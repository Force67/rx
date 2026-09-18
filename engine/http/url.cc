#include "http/url.h"

namespace rx::http {
namespace {

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

base::String Lowered(const base::String& text) {
  base::String out = text;
  for (base::String::size_type i = 0; i < out.size(); ++i)
    out[i] = Lower(out[i]);
  return out;
}

// Digits only, and nothing that would wrap a u16. A port of 0 is refused as
// well: it means "any port" to bind(), never anything to connect().
bool ParsePort(const base::String& text, u16* out) {
  if (text.empty() || text.size() > 5)
    return false;
  u32 value = 0;
  for (base::String::size_type i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9')
      return false;
    value = value * 10 + static_cast<u32>(c - '0');
  }
  if (value == 0 || value > 65535)
    return false;
  *out = static_cast<u16>(value);
  return true;
}

// CR and LF are the ones that matter (they end a request line), but nothing
// below 0x20 or at 0x7f belongs in a URL either.
bool HasControlChars(const base::String& text) {
  for (base::String::size_type i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == 0x7f)
      return true;
  }
  return false;
}

bool SchemePort(const base::String& scheme, u16* port, bool* tls) {
  if (scheme == "http") {
    *port = 80;
    *tls = false;
    return true;
  }
  if (scheme == "https") {
    *port = 443;
    *tls = true;
    return true;
  }
  return false;
}

// Where the authority stops: the first '/', '?' or '#'. Everything before it is
// host[:port], everything from it (minus any fragment) is the request target.
base::String::size_type AuthorityEnd(const base::String& text,
                                     base::String::size_type from) {
  for (base::String::size_type i = from; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '/' || c == '?' || c == '#')
      return i;
  }
  return text.size();
}

bool SplitAuthority(const base::String& authority,
                    const base::String& scheme,
                    base::String* host,
                    u16* port) {
  if (authority.empty())
    return false;
  // Credentials are refused, not stripped. See the note on Url.
  if (authority.find('@') != base::String::npos)
    return false;

  if (authority[0] == '[') {
    const auto close = authority.find(']');
    if (close == base::String::npos || close == 1)
      return false;
    *host = authority.substr(1, close - 1);
    // The caller already seeded `port` from the scheme, so a bracketed host
    // with no ":port" keeps that default.
    const base::String rest = authority.substr(close + 1);
    if (rest.empty())
      return true;
    return rest[0] == ':' && ParsePort(rest.substr(1), port);
  }

  const auto colon = authority.find(':');
  if (colon == base::String::npos) {
    *host = authority;
    return true;
  }
  // A second colon outside brackets is a bare IPv6 literal, which is ambiguous
  // with host:port and has to be written bracketed.
  if (authority.find(':', colon + 1) != base::String::npos)
    return false;
  *host = authority.substr(0, colon);
  return !host->empty() && ParsePort(authority.substr(colon + 1), port);
}

}  // namespace

bool Url::Parse(const base::String& text, Url* out) {
  // A CR or LF in a URL splices a second request into the stream once the
  // target reaches the request line, so it is refused here rather than
  // escaped later.
  if (HasControlChars(text))
    return false;

  const auto sep = text.find("://");
  if (sep == base::String::npos || sep == 0)
    return false;

  Url url;
  url.scheme = Lowered(text.substr(0, sep));
  if (!SchemePort(url.scheme, &url.port, &url.tls))
    return false;

  const auto authority_begin = sep + 3;
  const auto authority_end = AuthorityEnd(text, authority_begin);
  const base::String authority =
      text.substr(authority_begin, authority_end - authority_begin);
  if (!SplitAuthority(authority, url.scheme, &url.host, &url.port))
    return false;
  if (url.host.empty())
    return false;

  // The fragment is a client-side anchor and never reaches the wire.
  base::String target = text.substr(authority_end);
  const auto hash = target.find('#');
  if (hash != base::String::npos)
    target = target.substr(0, hash);
  if (target.empty() || target[0] == '?')
    target = base::String("/") + target;
  url.target = target;

  *out = url;
  return true;
}

base::String Url::Authority() const {
  const bool bracketed = host.find(':') != base::String::npos;
  base::String out;
  if (bracketed)
    out.push_back('[');
  out += host;
  if (bracketed)
    out.push_back(']');

  const bool default_port = (tls && port == 443) || (!tls && port == 80);
  if (!default_port) {
    out.push_back(':');
    char digits[8] = {};
    int n = 0;
    u16 value = port;
    do {
      digits[n++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (n-- > 0)
      out.push_back(digits[n]);
  }
  return out;
}

bool Url::Resolve(const base::String& location, Url* out) const {
  if (location.empty())
    return false;
  // A Location comes off the wire and its bytes end up on the next request
  // line, so it gets the screen Parse gives a URL. Without it a bare LF in a
  // header splices a second request into the stream, and a server that accepts
  // bare LF as a line ending reads whatever the sender wanted.
  if (HasControlChars(location))
    return false;

  // Absolute only when the scheme separator comes before any path, query or
  // fragment. Testing anywhere in the string sends "/login?next=https://x"
  // through the URL parser, which then reads "/login?next=https" as a scheme.
  const auto scheme_end = location.find("://");
  if (scheme_end != base::String::npos) {
    bool path_first = false;
    for (base::String::size_type i = 0; i < scheme_end; ++i) {
      const char c = location[i];
      if (c == '/' || c == '?' || c == '#') {
        path_first = true;
        break;
      }
    }
    if (!path_first)
      return Parse(location, out);
  }

  // Protocol-relative ("//cdn.example/x"): the scheme stays, everything else
  // comes from the location. Treating it as a path would send "GET //cdn..."
  // to the host we are already talking to.
  if (location.size() >= 2 && location[0] == '/' && location[1] == '/')
    return Parse(scheme + ":" + location, out);

  Url next = *this;
  if (location[0] == '/') {
    next.target = location;
  } else if (location[0] == '?') {
    // A query-only reference keeps the path and replaces the query.
    base::String base_path = target;
    const auto query = base_path.find('?');
    if (query != base::String::npos)
      base_path = base_path.substr(0, query);
    next.target = base_path + location;
  } else {
    // A relative reference replaces the last path segment, so "b" under
    // "/v1/a" is "/v1/b" and under "/v1/" is "/v1/b".
    base::String base_path = target;
    const auto query = base_path.find('?');
    if (query != base::String::npos)
      base_path = base_path.substr(0, query);
    const auto slash = base_path.find_last_of('/');
    base_path = slash == base::String::npos ? base::String("/")
                                            : base_path.substr(0, slash + 1);
    next.target = base_path + location;
  }
  const auto hash = next.target.find('#');
  if (hash != base::String::npos)
    next.target = next.target.substr(0, hash);
  *out = next;
  return true;
}

}  // namespace rx::http
