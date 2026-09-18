// rx::http acceptance: URL parsing and redirect resolution, response-head
// parsing, chunked decoding, and the refusals that keep a hostile string from
// becoming a second request. All pure parsing, no socket involved, so plain
// `ctest` runs it everywhere.
//
// The live half (a real connect, a real handshake) cannot run in ctest without
// reaching the network; RX_HTTP_LIVE=<url> drives it by hand:
//   RX_HTTP_LIVE=https://example.com ./build/linux/http_test

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#include <base/option.h>

#include "http/http.h"
#include "http/url.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace http = rx::http;
using rx::u16;
using rx::u32;

void TestUrlParse() {
  http::Url url;

  CHECK(http::Url::Parse("http://example.com", &url));
  CHECK_EQ(url.scheme, base::String("http"));
  CHECK_EQ(url.host, base::String("example.com"));
  CHECK_EQ(url.port, u16{80});
  CHECK_EQ(url.target, base::String("/"));
  CHECK(!url.tls);

  CHECK(http::Url::Parse("HTTPS://Example.com/v1/servers?domain=skyrim", &url));
  CHECK_EQ(url.scheme, base::String("https"));
  CHECK_EQ(url.port, u16{443});
  CHECK(url.tls);
  CHECK_EQ(url.target, base::String("/v1/servers?domain=skyrim"));

  CHECK(http::Url::Parse("http://127.0.0.1:8477/v1/stats", &url));
  CHECK_EQ(url.port, u16{8477});
  CHECK_EQ(url.Authority(), base::String("127.0.0.1:8477"));

  // The default port is left off the Host header, a non-default one is not.
  CHECK(http::Url::Parse("https://list.example:443/x", &url));
  CHECK_EQ(url.Authority(), base::String("list.example"));

  // An IPv6 literal must be bracketed, and comes back bracketed.
  CHECK(http::Url::Parse("http://[2001:db8::1]:8477/", &url));
  CHECK_EQ(url.host, base::String("2001:db8::1"));
  CHECK_EQ(url.port, u16{8477});
  CHECK_EQ(url.Authority(), base::String("[2001:db8::1]:8477"));

  // A query with no path still sends a valid request line.
  CHECK(http::Url::Parse("http://example.com?q=1", &url));
  CHECK_EQ(url.target, base::String("/?q=1"));

  // The fragment never reaches the wire.
  CHECK(http::Url::Parse("http://example.com/a#section", &url));
  CHECK_EQ(url.target, base::String("/a"));

  CHECK(!http::Url::Parse("example.com/x", &url));          // no scheme
  CHECK(!http::Url::Parse("ftp://example.com/x", &url));    // not ours to speak
  CHECK(!http::Url::Parse("http://", &url));                // no host
  CHECK(!http::Url::Parse("http://u:p@example.com", &url)); // credentials refused
  CHECK(!http::Url::Parse("http://example.com:0/", &url));  // port 0 is not dialable
  CHECK(!http::Url::Parse("http://example.com:99999/", &url));
  CHECK(!http::Url::Parse("http://2001:db8::1/", &url));    // unbracketed ipv6
  CHECK(!http::Url::Parse("http://example.com/a\r\nX: y", &url));  // request splitting
}

void TestUrlResolve() {
  http::Url base_url;
  CHECK(http::Url::Parse("https://list.example/v1/servers?x=1", &base_url));

  http::Url next;
  CHECK(base_url.Resolve("/v2/servers", &next));
  CHECK_EQ(next.host, base::String("list.example"));
  CHECK_EQ(next.target, base::String("/v2/servers"));
  CHECK(next.tls);

  CHECK(base_url.Resolve("http://other.example/x", &next));
  CHECK_EQ(next.host, base::String("other.example"));
  CHECK(!next.tls);

  // Relative: the last path segment is replaced, the query dropped.
  CHECK(base_url.Resolve("stats", &next));
  CHECK_EQ(next.target, base::String("/v1/stats"));

  CHECK(!base_url.Resolve("", &next));
  CHECK(!base_url.Resolve("gopher://example.com/x", &next));
  // A control byte in a Location ends up on the next request line.
  CHECK(!base_url.Resolve("/next\nX-Injected: yes", &next));
  CHECK(!base_url.Resolve("/next\rX-Injected: yes", &next));

  // "://" inside a query does not make the location absolute: this shape is
  // every login gateway on the web.
  CHECK(base_url.Resolve("/login?next=https://app.example/x", &next));
  CHECK_EQ(next.host, base::String("list.example"));
  CHECK_EQ(next.target, base::String("/login?next=https://app.example/x"));

  // Protocol-relative keeps the scheme and changes the host.
  CHECK(base_url.Resolve("//cdn.example/asset.js", &next));
  CHECK_EQ(next.host, base::String("cdn.example"));
  CHECK(next.tls);
  CHECK_EQ(next.target, base::String("/asset.js"));

  // A query-only reference keeps the path.
  CHECK(base_url.Resolve("?page=2", &next));
  CHECK_EQ(next.target, base::String("/v1/servers?page=2"));
}

void TestResponseHead() {
  http::Response response;
  const base::String head =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: 27\r\n"
      "X-Odd\r\n"  // junk without a colon: skipped, not fatal
      "Server: axum\r\n";
  CHECK(http::ParseResponseHead(head, &response));
  CHECK_EQ(response.status, u16{200});
  CHECK(response.ok());
  CHECK_EQ(response.headers.size(), size_t{3});

  // Lookup is case-insensitive, and the value is trimmed.
  const base::String* type = response.Find("CONTENT-TYPE");
  CHECK(type != nullptr && *type == base::String("application/json"));
  CHECK(response.Find("nothing-here") == nullptr);

  http::Response created;
  CHECK(http::ParseResponseHead("HTTP/1.1 201 Created\r\nLocation: /v1/x\r\n", &created));
  CHECK_EQ(created.status, u16{201});
  CHECK(created.ok());  // the whole 2xx range counts, not just 200

  http::Response minimal;
  CHECK(http::ParseResponseHead("HTTP/1.0 204 ", &minimal));
  CHECK_EQ(minimal.status, u16{204});

  http::Response bad;
  CHECK(!http::ParseResponseHead("220 smtp.example ESMTP", &bad));
  CHECK(!http::ParseResponseHead("HTTP/1.1 99 Nope\r\n", &bad));
  CHECK(!http::ParseResponseHead("HTTP/1.1 twenty\r\n", &bad));
}

void TestChunked() {
  base::String body;
  CHECK(http::DecodeChunked("4\r\nrx::\r\n4\r\nhttp\r\n0\r\n\r\n", &body, 1024));
  CHECK_EQ(body, base::String("rx::http"));

  // Chunk extensions are ignored, trailers are skipped.
  CHECK(http::DecodeChunked("5;x=1\r\nhello\r\n0\r\nX-Sum: 3\r\n\r\n", &body, 1024));
  CHECK_EQ(body, base::String("hello"));

  CHECK(http::DecodeChunked("0\r\n\r\n", &body, 1024));
  CHECK(body.empty());

  // Incomplete is not done: the client reads more rather than truncating.
  CHECK(!http::DecodeChunked("4\r\nrx::\r\n", &body, 1024));
  CHECK(!http::DecodeChunked("4\r\nrx", &body, 1024));
  // Malformed and over-cap both fail.
  CHECK(!http::DecodeChunked("zz\r\nrx\r\n0\r\n\r\n", &body, 1024));
  CHECK(!http::DecodeChunked("8\r\nrx::http\r\n0\r\n\r\n", &body, 4));
}

void TestRequestRefusals() {
  // Bad input is refused before a socket is opened, so these are instant and
  // need no network: status stays 0 and the reason is in `error`.
  http::Request request;
  request.url = "http://example.com/a";
  request.headers.push_back(http::Header{"X-Evil", "a\r\nHost: elsewhere"});
  const http::Response injected = http::Fetch(request);
  CHECK_EQ(injected.status, u16{0});
  CHECK(!injected.error.empty());

  const http::Response unparsable = http::Get("not a url");
  CHECK_EQ(unparsable.status, u16{0});
  CHECK(!unparsable.error.empty());

  // An https URL on a build without a TLS backend must fail, never downgrade.
  if (!http::TlsAvailable()) {
    const http::Response no_tls = http::Get("https://example.com/");
    CHECK_EQ(no_tls.status, u16{0});
    CHECK(!no_tls.error.empty());
  }
}

// A server on loopback that answers one request with bytes the test chose, so
// the paths BELOW the parsers (the body loops, the redirect loop, the framing
// rules) can be driven without reaching the network. Everything here runs in
// plain ctest.
class CannedServer {
 public:
  // `reply` goes out verbatim. `reset` closes the connection with RST instead
  // of a clean FIN, which is how a truncated body arrives in the wild.
  // `slice` > 0 dribbles the reply out in pieces instead of one write, which is
  // what makes the client read a body across several calls: the chunked decoder
  // resumes from a cursor, and only a sliced body exercises that.
  CannedServer(base::String reply, bool reset = false, size_t slice = 0)
      : reply_(std::move(reply)), reset_(reset), slice_(slice) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    const int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // the kernel picks a free one
    ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listener_, 4);
    socklen_t len = sizeof(addr);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { Serve(); });
  }

  ~CannedServer() {
    if (thread_.joinable())
      thread_.join();
    ::close(listener_);
  }

  base::String url(const char* path = "/") const {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "http://127.0.0.1:%u%s", unsigned(port_), path);
    return base::String(buffer);
  }

  // What the client actually put on the wire, for the injection checks.
  base::String request() {
    if (thread_.joinable())
      thread_.join();
    return request_;
  }

 private:
  void Serve() {
    const int client = ::accept(listener_, nullptr, nullptr);
    if (client < 0)
      return;
    char buffer[8192];
    const auto got = ::recv(client, buffer, sizeof(buffer), 0);
    if (got > 0)
      request_.append(buffer, static_cast<base::String::size_type>(got));
    if (slice_ == 0) {
      ::send(client, reply_.c_str(), reply_.size(), MSG_NOSIGNAL);
    } else {
      for (size_t at = 0; at < reply_.size(); at += slice_) {
        const size_t left = reply_.size() - at;
        ::send(client, reply_.c_str() + at, left < slice_ ? left : slice_, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (reset_) {
      linger hard{};
      hard.l_onoff = 1;
      hard.l_linger = 0;  // close() now sends RST, not FIN
      ::setsockopt(client, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
    }
    ::close(client);
  }

  base::String reply_;
  bool reset_ = false;
  size_t slice_ = 0;
  int listener_ = -1;
  u16 port_ = 0;
  std::thread thread_;
  base::String request_;
};

void TestBodyFraming() {
  {
    CannedServer server(
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{200});
    CHECK_EQ(r.body, base::String("hello"));
  }
  {
    // A body with no framing ends at a CLEAN close, and only then.
    CannedServer server("HTTP/1.1 200 OK\r\n\r\nhello");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{200});
    CHECK_EQ(r.body, base::String("hello"));
  }
  {
    // The same shape cut off by an RST is NOT a 200: a truncated body that
    // reads as success is the truncation attack framing exists to catch.
    CannedServer server("HTTP/1.1 200 OK\r\n\r\n{\"servers\":[{\"a", true);
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{0});
    CHECK(!r.error.empty());
    CHECK(r.body.empty());
  }
  {
    // A declared length the server does not deliver fails too.
    CannedServer server("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{0});
  }
  {
    // An interim answer is not the response.
    CannedServer server(
        "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nHI");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{200});
    CHECK_EQ(r.body, base::String("HI"));
  }
  {
    // A Content-Length that is not a length is a broken message, not a hint to
    // read until close.
    CannedServer server("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\nSNEAKY");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{0});
  }
  {
    // Two lengths let two readers disagree about where the message ends.
    CannedServer server(
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nContent-Length: 12\r\n\r\nAAAABBBBCCCC");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{0});
  }
  {
    CannedServer server(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nrx::\r\n4\r\nhttp\r\n0\r\n\r\n");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{200});
    CHECK_EQ(r.body, base::String("rx::http"));
  }
  {
    // The same body arriving 7 bytes at a time: every read resumes the decoder
    // rather than starting it over.
    CannedServer server(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nrx::\r\n4\r\nhttp\r\n5\r\n/1.1!\r\n0\r\n\r\n",
        false, 7);
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{200});
    CHECK_EQ(r.body, base::String("rx::http/1.1!"));
  }
  {
    // 16 MB in 16 KB chunks. Decoding from offset zero on every socket read is
    // quadratic: this body took 7.3 s of pure CPU that way, and nothing would
    // have stopped it because none of that time is spent in a socket call. The
    // cursor makes it linear, so the budget below is 20x what it now costs and
    // still an order of magnitude under the old behavior.
    const size_t kChunk = 16 * 1024;
    const size_t kChunks = 1024;
    base::String reply = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const base::String piece(kChunk, 'x');
    for (size_t i = 0; i < kChunks; ++i) {
      reply += "4000\r\n";  // 16384 in hex
      reply += piece;
      reply += "\r\n";
    }
    reply += "0\r\n\r\n";
    CannedServer server(reply, false, 512 * 1024);

    http::Request request;
    request.url = server.url();
    request.max_body_bytes = 32u * 1024 * 1024;  // http.h invites raising it
    const auto began = std::chrono::steady_clock::now();
    const http::Response r = http::Fetch(request);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();
    CHECK_EQ(r.status, u16{200});
    CHECK_EQ(r.body.size(), kChunk * kChunks);
    CHECK(took < 3000);
    if (took >= 3000)
      std::printf("  chunked 16 MB took %lld ms\n", static_cast<long long>(took));
  }
  {
    // A chunk that declares 2^64-1 bytes: the guards have to survive the
    // arithmetic, not wrap into a memcpy of the whole address space.
    CannedServer server(
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nrx::\r\nffffffffffffffff\r\nA");
    const http::Response r = http::Get(server.url());
    CHECK_EQ(r.status, u16{0});
    CHECK(r.body.empty());
  }
}

void TestRedirectSafety() {
  {
    // Credentials are addressed to one origin. A Location pointing elsewhere
    // must not carry them along.
    CannedServer attacker("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    base::String reply = "HTTP/1.1 302 Found\r\nLocation: ";
    reply += attacker.url("/stolen");
    reply += "\r\nContent-Length: 0\r\n\r\n";
    CannedServer origin(reply);

    http::Request request;
    request.url = origin.url();
    request.headers.push_back(http::Header{"Authorization", "Bearer SECRET"});
    request.headers.push_back(http::Header{"Cookie", "session=abc"});
    request.headers.push_back(http::Header{"X-Keep", "fine"});
    const http::Response r = http::Fetch(request);
    CHECK_EQ(r.status, u16{200});

    const base::String seen = attacker.request();
    CHECK(seen.find("SECRET") == base::String::npos);
    CHECK(seen.find("session=abc") == base::String::npos);
    CHECK(seen.find("X-Keep") != base::String::npos);  // an ordinary header stays
  }
  {
    // A bare LF in a Location would otherwise be spliced onto the next
    // request line, where a lenient server reads it as a second header.
    CannedServer origin(
        "HTTP/1.1 302 Found\r\nLocation: /next\nX-Injected: yes\r\nContent-Length: 0\r\n\r\n");
    const http::Response r = http::Get(origin.url());
    CHECK_EQ(r.status, u16{0});
    CHECK(!r.error.empty());
  }
  {
    // Following a redirect off TLS would put a token on the wire in clear. We
    // cannot serve https here, so the rule is checked where it is decided.
    http::Url https;
    CHECK(http::Url::Parse("https://list.example/v1/servers", &https));
    http::Url next;
    CHECK(https.Resolve("http://plain.example/x", &next));
    CHECK(!next.tls);  // Resolve allows it; Fetch is what refuses the hop
  }
}

// A server that accepts and then says nothing: the shape that used to park a
// worker thread until the process was killed.
class SilentServer {
 public:
  SilentServer() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    const int on = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(listener_, 4);
    socklen_t len = sizeof(addr);
    ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] {
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0)
        return;
      // Hold the connection open, saying nothing, until the test is done.
      while (!done_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      ::close(client);
    });
  }

  ~SilentServer() {
    done_ = true;
    if (thread_.joinable())
      thread_.join();
    ::close(listener_);
  }

  base::String url() const {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "http://127.0.0.1:%u/", unsigned(port_));
    return base::String(buffer);
  }

 private:
  int listener_ = -1;
  u16 port_ = 0;
  std::atomic<bool> done_{false};
  std::thread thread_;
};

void TestDeadline() {
  SilentServer server;
  http::Request request;
  request.url = server.url();
  request.timeout_ms = 30'000;      // the idle timeout alone would hold for 30 s
  request.total_timeout_ms = 700;   // the deadline is what ends this

  const auto began = std::chrono::steady_clock::now();
  const http::Response r = http::Fetch(request);
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - began)
                        .count();
  CHECK_EQ(r.status, u16{0});
  CHECK(!r.cancelled);  // it ran out of time, nobody cancelled it
  CHECK(took >= 600 && took < 3000);
  if (took >= 3000)
    std::printf("  deadline took %lld ms\n", static_cast<long long>(took));
}

void TestCancel() {
  SilentServer server;
  std::atomic<bool> cancel{false};

  http::Request request;
  request.url = server.url();
  request.timeout_ms = 30'000;
  request.total_timeout_ms = 30'000;  // neither limit is what ends this
  request.cancel = &cancel;

  // What a shutdown does: raise the flag on another thread and join.
  std::thread raiser([&cancel] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cancel = true;
  });

  const auto began = std::chrono::steady_clock::now();
  const http::Response r = http::Fetch(request);
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - began)
                        .count();
  raiser.join();

  CHECK_EQ(r.status, u16{0});
  CHECK(r.cancelled);  // the caller's doing, not the peer's: do not log a fault
  // Within a wake tick or so of the flag going up, not 30 seconds later.
  CHECK(took < 2000);
  if (took >= 2000)
    std::printf("  cancel took %lld ms\n", static_cast<long long>(took));

  // A flag already raised means the call never opens a socket at all.
  std::atomic<bool> already{true};
  http::Request second = request;
  second.cancel = &already;
  const http::Response none = http::Fetch(second);
  CHECK_EQ(none.status, u16{0});
  CHECK(none.cancelled);
}

// Off by default: reaches the network, so it only runs when asked.
void TestLive() {
  const char* target = std::getenv("RX_HTTP_LIVE");
  if (target == nullptr || *target == '\0')
    return;
  std::printf("http_test: live fetch of %s (tls backend: %s)\n", target,
              http::TlsAvailable() ? "yes" : "no");
  const http::Response response = http::Get(target);
  std::printf("  status %u, %zu bytes, error '%s'\n", unsigned(response.status),
              size_t(response.body.size()), response.error.c_str());
  // The first line or so of the body, which is how a probe endpoint (say
  // howsmyssl) is read back: the answer is the point of the fetch. The whole
  // body goes to RX_HTTP_LIVE_OUT when the interesting part is further in.
  if (!response.body.empty()) {
    const size_t shown = response.body.size() < 240 ? response.body.size() : 240;
    std::printf("  body: %.*s\n", int(shown), response.body.c_str());
    if (const char* out = std::getenv("RX_HTTP_LIVE_OUT"); out != nullptr && *out != '\0') {
      if (std::FILE* file = std::fopen(out, "wb"); file != nullptr) {
        std::fwrite(response.body.c_str(), 1, response.body.size(), file);
        std::fclose(file);
        std::printf("  body written to %s\n", out);
      }
    }
  }
  CHECK(response.ok());
  CHECK(!response.body.empty());
}

}  // namespace

int main() {
  // The client's own knobs (RX_HTTP_TLS_DEBUG) are base::Options, and nothing
  // else in a bare test binary populates them from the environment.
  base::InitOptionsFromEnv();
  TestUrlParse();
  TestUrlResolve();
  TestResponseHead();
  TestChunked();
  TestRequestRefusals();
  TestBodyFraming();
  TestRedirectSafety();
  TestDeadline();
  TestCancel();
  TestLive();
  if (g_failures == 0) {
    std::printf("http_test: all passed\n");
    return 0;
  }
  std::printf("http_test: %d failure(s)\n", g_failures);
  return 1;
}
