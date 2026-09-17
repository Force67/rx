#include "http/http.h"

#include <cstdio>
#include <initializer_list>

#include "http/stream.h"

namespace rx::http {
namespace {

// A response head larger than this is a server doing something else entirely.
// The cap bounds the buffer a peer can grow before it has sent a status line.
constexpr u32 kMaxHeadBytes = 64 * 1024;
constexpr u32 kReadChunk = 16 * 1024;

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreCase(const base::String& a, const base::String& b) {
  if (a.size() != b.size())
    return false;
  for (base::String::size_type i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i]))
      return false;
  return true;
}

// `needle` must already be lowercase: only the haystack is folded.
bool Contains(const base::String& haystack, const char* lowercase_needle) {
  const base::String needle(lowercase_needle);
  if (needle.empty() || haystack.size() < needle.size())
    return false;
  for (base::String::size_type i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool hit = true;
    for (base::String::size_type j = 0; j < needle.size(); ++j) {
      if (Lower(haystack[i + j]) != needle[j]) {
        hit = false;
        break;
      }
    }
    if (hit)
      return true;
  }
  return false;
}

base::String Trim(const base::String& text) {
  base::String::size_type begin = 0;
  base::String::size_type end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
    ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
    --end;
  return text.substr(begin, end - begin);
}

base::String Decimal(u64 value) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return base::String(buffer);
}

// A CR or LF anywhere in a method, URL or header would let a caller splice a
// second request into the stream. Refused at the door instead of escaped.
bool HasControlChars(const base::String& text) {
  for (base::String::size_type i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == 0x7f)
      return true;
  }
  return false;
}

bool ParseDecimal(const base::String& text, u64* out) {
  if (text.empty())
    return false;
  u64 value = 0;
  for (base::String::size_type i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9')
      return false;
    if (value > (~u64{0} - 9) / 10)
      return false;
    value = value * 10 + static_cast<u64>(c - '0');
  }
  *out = value;
  return true;
}

// Chunk sizes are hex and may carry a ";extension" this client ignores.
bool ParseChunkSize(const base::String& text,
                    base::String::size_type begin,
                    base::String::size_type end,
                    u64* out) {
  u64 value = 0;
  bool any = false;
  for (auto i = begin; i < end; ++i) {
    const char c = text[i];
    if (c == ';' || c == ' ' || c == '\t')
      break;
    int digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return false;
    if (value > (~u64{0} >> 4))
      return false;
    value = value * 16 + static_cast<u64>(digit);
    any = true;
  }
  *out = value;
  return any;
}

enum class Chunked { kNeedMore, kDone, kBad };

// How far into `raw` the decoder got, so the next call resumes instead of
// starting over. Decoding from zero on every socket read is quadratic in the
// body size: a 64 MB chunked answer cost 210 seconds of pure CPU, and no
// timeout covers it because none of that time is spent in a socket call.
struct ChunkedCursor {
  base::String::size_type consumed = 0;  // bytes of `raw` already decoded
};

// Decodes what has arrived since the last call. kNeedMore means read more and
// call again with the longer buffer and the same cursor.
Chunked DecodeChunkedPartial(const base::String& raw,
                             base::String* out,
                             u32 max_bytes,
                             ChunkedCursor* cursor) {
  base::String::size_type i = cursor->consumed;
  for (;;) {
    const auto eol = raw.find("\r\n", i);
    if (eol == base::String::npos)
      return Chunked::kNeedMore;

    u64 size = 0;
    if (!ParseChunkSize(raw, i, eol, &size))
      return Chunked::kBad;

    if (size == 0) {
      // The terminating chunk, then optional trailers, then a blank line.
      base::String::size_type trailer = eol + 2;
      for (;;) {
        const auto line_end = raw.find("\r\n", trailer);
        if (line_end == base::String::npos)
          return Chunked::kNeedMore;
        if (line_end == trailer)
          return Chunked::kDone;
        trailer = line_end + 2;
      }
    }

    // Written as a subtraction because the sum overflows: a declared size of
    // 0xffffffffffffffff made `out->size() + size` wrap past the cap and
    // `i + size + 2` wrap past the buffer length, and the append that followed
    // was a memcpy of 2^64 bytes.
    if (size > static_cast<u64>(max_bytes) - out->size())
      return Chunked::kBad;
    const auto data = eol + 2;
    if (raw.size() < data + size + 2)
      return Chunked::kNeedMore;
    out->append(raw.c_str() + data, static_cast<base::String::size_type>(size));
    i = data + static_cast<base::String::size_type>(size);
    if (raw[i] != '\r' || raw[i + 1] != '\n')
      return Chunked::kBad;
    i += 2;
    cursor->consumed = i;  // this chunk is in `out` and will not be re-read
  }
}

// Removes every header whose name matches one of `names` (already lowercase).
void DropHeaders(base::Vector<Header>* headers,
                 std::initializer_list<const char*> names) {
  base::Vector<Header> kept;
  for (const Header& header : *headers) {
    bool drop = false;
    for (const char* name : names) {
      if (EqualsIgnoreCase(header.name, base::String(name))) {
        drop = true;
        break;
      }
    }
    if (!drop)
      kept.push_back(header);
  }
  *headers = kept;
}

bool IsRedirect(u16 status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

bool HasHeader(const base::Vector<Header>& headers, const char* name) {
  const base::String wanted(name);
  for (const Header& header : headers)
    if (EqualsIgnoreCase(header.name, wanted))
      return true;
  return false;
}

// The request bytes, in the order a server logs them. Caller-supplied headers
// win over the defaults, so an endpoint that wants its own Accept or
// User-Agent gets it.
base::String BuildRequest(const Request& request,
                          const Url& url,
                          const base::String& method,
                          const base::String& body) {
  base::String out;
  out += method;
  out += " ";
  out += url.target;
  out += " HTTP/1.1\r\n";
  if (!HasHeader(request.headers, "host")) {
    out += "Host: ";
    out += url.Authority();
    out += "\r\n";
  }
  if (!HasHeader(request.headers, "user-agent"))
    out += "User-Agent: rx-http/1.0\r\n";
  if (!HasHeader(request.headers, "accept"))
    out += "Accept: */*\r\n";
  // One exchange per connection: no pool to manage, and the server closes the
  // socket, which doubles as the end-of-body signal when there is no length.
  out += "Connection: close\r\n";

  if (!body.empty() && !request.content_type.empty() &&
      !HasHeader(request.headers, "content-type")) {
    out += "Content-Type: ";
    out += request.content_type;
    out += "\r\n";
  }
  // A method that can carry a body says how long it is even when it is empty:
  // without the header nginx answers 411 rather than running the request.
  const bool body_allowed = !EqualsIgnoreCase(method, base::String("GET")) &&
                            !EqualsIgnoreCase(method, base::String("HEAD"));
  if ((!body.empty() || body_allowed) && !HasHeader(request.headers, "content-length")) {
    out += "Content-Length: ";
    out += Decimal(body.size());
    out += "\r\n";
  }

  for (const Header& header : request.headers) {
    if (EqualsIgnoreCase(header.name, base::String("connection")))
      continue;  // this client decides the connection lifetime
    out += header.name;
    out += ": ";
    out += header.value;
    out += "\r\n";
  }
  out += "\r\n";
  out += body;
  return out;
}

// One hop: connect, send, read the answer. Redirects are the caller's job.
Response Exchange(const Request& request,
                  const Url& url,
                  const base::String& method,
                  const base::String& body) {
  Response response;

  std::unique_ptr<Stream> stream;
  if (url.tls) {
    TlsOptions options;
    options.ca_file = request.ca_file;
    options.verify_peer = request.verify_peer;
    stream = MakeTlsStream(options);
    if (!stream) {
      response.error =
          "this build has no tls backend (RX_HTTP_TLS off), so https is unreachable";
      return response;
    }
  } else {
    stream = MakeTcpStream();
  }

  // 0 means "return immediately" to poll and "wait forever" to SO_RCVTIMEO, and
  // anything past INT_MAX turns into a negative poll timeout, which is also
  // "wait forever". Neither is a timeout, so the value is bounded here.
  const u32 timeout_ms =
      request.timeout_ms == 0 ? 1u : (request.timeout_ms > 3600000u ? 3600000u
                                                                   : request.timeout_ms);
  if (!stream->Connect(url.host, url.port, timeout_ms, &response.error))
    return response;

  const base::String wire = BuildRequest(request, url, method, body);
  if (!stream->Write(wire.c_str(), static_cast<u32>(wire.size()), &response.error))
    return response;

  // Head first: read until the blank line that ends it, keeping whatever body
  // bytes arrived in the same packets.
  base::String buffer;
  base::String::size_type head_end = base::String::npos;
  char chunk[kReadChunk];
  while (head_end == base::String::npos) {
    const i64 got = stream->Read(chunk, kReadChunk, &response.error);
    if (got < 0)
      return response;
    if (got == 0) {
      response.error = buffer.empty() ? base::String("the server closed the connection "
                                                     "without answering")
                                      : base::String("the response head was cut short");
      return response;
    }
    buffer.append(chunk, static_cast<base::String::size_type>(got));
    head_end = buffer.find("\r\n\r\n");
    if (head_end == base::String::npos && buffer.size() > kMaxHeadBytes) {
      response.error = "response head exceeds 64 KB";
      return response;
    }
  }

  // A 1xx is an interim answer, not the response: an unsolicited "100 Continue"
  // is legal and some proxies emit one. Drop it and read the next head, or the
  // real response ends up inside this one's body.
  for (;;) {
    if (!ParseResponseHead(buffer.substr(0, head_end), &response)) {
      response.error = "the answer is not http (no status line)";
      response.status = 0;
      return response;
    }
    if (response.status < 100 || response.status >= 200)
      break;
    response = Response{};
    buffer = buffer.substr(head_end + 4);
    head_end = buffer.find("\r\n\r\n");
    while (head_end == base::String::npos) {
      const i64 got = stream->Read(chunk, kReadChunk, &response.error);
      if (got <= 0) {
        response.error = "the server sent an interim answer and then stopped";
        response.status = 0;
        return response;
      }
      buffer.append(chunk, static_cast<base::String::size_type>(got));
      head_end = buffer.find("\r\n\r\n");
      if (head_end == base::String::npos && buffer.size() > kMaxHeadBytes) {
        response.error = "response head exceeds 64 KB";
        response.status = 0;
        return response;
      }
    }
  }

  base::String raw = buffer.substr(head_end + 4);

  // 204 and 304 carry no body by definition, and neither does a HEAD reply;
  // reading on would block until the server closes.
  const bool bodyless = response.status == 204 || response.status == 304 ||
                        EqualsIgnoreCase(method, base::String("HEAD"));
  if (bodyless)
    return response;

  const base::String* encoding = response.Find(base::String("transfer-encoding"));
  const bool chunked = encoding != nullptr && Contains(*encoding, "chunked");
  const base::String* length_header = response.Find(base::String("content-length"));

  if (chunked) {
    ChunkedCursor cursor;
    for (;;) {
      const Chunked state =
          DecodeChunkedPartial(raw, &response.body, request.max_body_bytes, &cursor);
      if (state == Chunked::kDone)
        return response;
      if (state == Chunked::kBad) {
        response.error = "malformed chunked body";
        response.status = 0;
        response.body.clear();  // partial bytes with no status is a trap
        return response;
      }
      const i64 got = stream->Read(chunk, kReadChunk, &response.error);
      if (got < 0) {
        response.status = 0;
        return response;
      }
      if (got == 0) {
        response.error = "the chunked body was cut short";
        response.status = 0;
        return response;
      }
      raw.append(chunk, static_cast<base::String::size_type>(got));
      if (raw.size() > request.max_body_bytes + kMaxHeadBytes) {
        response.error = "response body exceeds the limit";
        response.status = 0;
        return response;
      }
    }
  }

  u64 expected = 0;
  bool have_length = false;
  if (length_header != nullptr) {
    // RFC 7230: a message with an unparseable Content-Length must be rejected.
    // Falling through to "read until close" instead made "Content-Length: -1"
    // and a 24-digit length return a happy 200 with whatever arrived.
    if (!ParseDecimal(Trim(*length_header), &expected)) {
      response.error = "the server sent a content-length that is not a length";
      response.status = 0;
      return response;
    }
    // Two different lengths mean two readers can disagree about where this
    // message ends, which is how a request smuggles past a proxy.
    for (const Header& header : response.headers) {
      if (!EqualsIgnoreCase(header.name, base::String("content-length")))
        continue;
      u64 other = 0;
      if (!ParseDecimal(Trim(header.value), &other) || other != expected) {
        response.error = "the server sent conflicting content-lengths";
        response.status = 0;
        return response;
      }
    }
    have_length = true;
  }
  if (have_length && expected > request.max_body_bytes) {
    response.error = "response body exceeds the limit";
    response.status = 0;
    return response;
  }

  for (;;) {
    if (have_length && raw.size() >= expected)
      break;
    if (!have_length && raw.size() > request.max_body_bytes) {
      response.error = "response body exceeds the limit";
      response.status = 0;
      return response;
    }
    const i64 got = stream->Read(chunk, kReadChunk, &response.error);
    if (got < 0) {
      // Only a clean end of stream closes a body with no declared length. A
      // read error is not one: treating a reset mid-body as "the end" handed
      // the caller a truncated JSON document as a successful 200, which over
      // TLS is the truncation attack this framing exists to notice.
      response.status = 0;
      response.body.clear();
      return response;
    }
    if (got == 0) {
      if (have_length && raw.size() < expected) {
        response.error = "the body was cut short";
        response.status = 0;
        return response;
      }
      break;
    }
    raw.append(chunk, static_cast<base::String::size_type>(got));
  }

  if (have_length && raw.size() > expected)
    raw = raw.substr(0, static_cast<base::String::size_type>(expected));
  response.body = raw;
  return response;
}

}  // namespace

const base::String* Response::Find(const base::String& name) const {
  for (const Header& header : headers)
    if (EqualsIgnoreCase(header.name, name))
      return &header.value;
  return nullptr;
}

bool ParseResponseHead(const base::String& head, Response* out) {
  auto eol = head.find("\r\n");
  const base::String status_line = eol == base::String::npos ? head : head.substr(0, eol);
  // "HTTP/1.1 200 OK": version, space, three digits, and whatever reason
  // phrase the server felt like (it carries no meaning and is ignored).
  if (status_line.size() < 12 || status_line.substr(0, 5) != base::String("HTTP/"))
    return false;
  const auto space = status_line.find(' ');
  if (space == base::String::npos || space + 3 >= status_line.size())
    return false;
  u64 code = 0;
  if (!ParseDecimal(status_line.substr(space + 1, 3), &code) || code < 100 || code > 599)
    return false;
  out->status = static_cast<u16>(code);

  while (eol != base::String::npos) {
    const auto begin = eol + 2;
    if (begin >= head.size())
      break;
    const auto next = head.find("\r\n", begin);
    const base::String line =
        next == base::String::npos ? head.substr(begin) : head.substr(begin, next - begin);
    eol = next;
    const auto colon = line.find(':');
    // A line without a colon is junk from a broken server; skipping it beats
    // failing a response whose headers are otherwise fine.
    if (colon == base::String::npos || colon == 0)
      continue;
    out->headers.push_back(Header{Trim(line.substr(0, colon)), Trim(line.substr(colon + 1))});
  }
  return true;
}

bool DecodeChunked(const base::String& raw, base::String* out, u32 max_bytes) {
  out->clear();
  ChunkedCursor cursor;
  return DecodeChunkedPartial(raw, out, max_bytes, &cursor) == Chunked::kDone;
}

bool TlsAvailable() {
  TlsOptions options;
  return MakeTlsStream(options) != nullptr;
}

Response Fetch(const Request& request) {
  Response response;

  const base::String method = request.method.empty() ? base::String("GET") : request.method;
  if (HasControlChars(method) || HasControlChars(request.url)) {
    response.error = "the method or url carries control characters";
    return response;
  }
  for (const Header& header : request.headers) {
    if (header.name.empty() || HasControlChars(header.name) ||
        HasControlChars(header.value)) {
      response.error = "a request header carries control characters";
      return response;
    }
  }

  Url url;
  if (!Url::Parse(request.url, &url)) {
    response.error = base::String("not a url this client can dial: ") + request.url;
    return response;
  }

  // The request each hop actually sends. It starts as the caller's and is
  // stripped down as the redirects move it away from the origin it was
  // addressed to.
  Request hop = request;
  hop.method = method;
  u32 redirects_left = request.max_redirects;
  for (;;) {
    response = Exchange(hop, url, hop.method, hop.body);
    if (response.status == 0 || !IsRedirect(response.status))
      return response;
    if (redirects_left == 0) {
      // Say so, rather than handing back a 3xx that looks like the server's
      // final answer. A caller that switched following off (a POST, where a
      // redirect would silently become a GET) needs to hear which it was.
      const base::String* to = response.Find(base::String("location"));
      response.error = request.max_redirects == 0
                           ? base::String("the server redirected to ") +
                                 (to != nullptr ? *to : base::String("elsewhere")) +
                                 ", which this request does not follow"
                           : base::String("too many redirects");
      response.status = 0;
      return response;
    }

    const base::String* location = response.Find(base::String("location"));
    if (location == nullptr || location->empty())
      return response;  // a redirect with nowhere to go is the answer itself

    Url next;
    if (!url.Resolve(*location, &next)) {
      response.error = base::String("cannot follow the redirect to ") + *location;
      response.status = 0;
      return response;
    }
    // A redirect off TLS is refused, never followed: the caller asked for https
    // and the bytes (a token, a session) would go out in the clear.
    if (url.tls && !next.tls) {
      response.error = "the redirect leaves https for http";
      response.status = 0;
      return response;
    }
    // Credentials belong to the origin they were addressed to. Every browser
    // and every serious client drops them when a redirect crosses to another
    // host, port or scheme, and so does this: otherwise one hostile Location
    // hands a third party the caller's Authorization header. A caller-supplied
    // Host goes for the same reason (it would name the old vhost on the new
    // machine).
    const bool same_origin =
        next.host == url.host && next.port == url.port && next.tls == url.tls;
    if (!same_origin)
      DropHeaders(&hop.headers, {"authorization", "cookie", "proxy-authorization", "host"});

    // 303 says so outright, and 301/302 are treated the same way because that
    // is what every client on the web does; 307/308 exist to keep the method.
    if (response.status == 301 || response.status == 302 || response.status == 303) {
      hop.method = "GET";
      hop.body.clear();
      // The body is gone, so its framing headers have to go with it, or the
      // next server waits for bytes that will never arrive.
      DropHeaders(&hop.headers, {"content-length", "content-type", "transfer-encoding"});
    }
    url = next;
    --redirects_left;
  }
}

Response Get(const base::String& url) {
  Request request;
  request.url = url;
  return Fetch(request);
}

Response PostJson(const base::String& url, const base::String& json) {
  Request request;
  request.method = "POST";
  request.url = url;
  request.body = json;
  request.content_type = "application/json";
  return Fetch(request);
}

}  // namespace rx::http
