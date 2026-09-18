#ifndef RX_HTTP_URL_H_
#define RX_HTTP_URL_H_

#include <base/strings/xstring.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::http {

// An absolute URL reduced to what a client needs to dial it: which transport,
// where to connect, and the string that goes on the request line. Userinfo
// ("http://user:pass@host") is refused rather than dropped, because a URL that
// carries credentials is not the URL the caller thinks it is once they are
// silently discarded.
struct RX_HTTP_EXPORT Url {
  base::String scheme;       // lowercased; only "http" and "https" parse
  base::String host;         // an IPv6 literal is stored without its brackets
  u16 port = 0;              // taken from the scheme when the URL omits one
  base::String target{"/"};  // path + query, byte for byte what is sent
  bool tls = false;

  // False on anything it cannot dial, leaving `out` untouched. The caller
  // reports the text it was handed: a mistyped endpoint is the common case and
  // the string is the only useful part of the message.
  static bool Parse(const base::String& text, Url* out);

  // The Host header value: bare host when the port is the scheme default,
  // "host:port" otherwise, brackets restored around an IPv6 literal.
  base::String Authority() const;

  // Resolves a Location against this URL: absolute ("https://other/x"),
  // rooted ("/x"), or relative ("x"). False when the result is not dialable,
  // which includes a redirect to a scheme this client does not speak.
  bool Resolve(const base::String& location, Url* out) const;
};

}  // namespace rx::http

#endif  // RX_HTTP_URL_H_
