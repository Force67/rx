#ifndef RX_HTTP_HTTP_H_
#define RX_HTTP_HTTP_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/export.h"
#include "core/types.h"
#include "http/url.h"

// A small HTTP/1.1 client: one request, one connection, one answer. It exists
// so the engine can talk to plain web APIs (a server list, an update check)
// without pulling a dependency in; it is not a general-purpose library and
// deliberately has no keep-alive pool, no cookie jar and no compression.
//
// Every call BLOCKS the calling thread for as long as the exchange takes. Never
// call it from a frame thread: hand it to a worker and pick the result up.
//
// https rides mbedTLS (RX_HTTP_TLS), TLS 1.3 with a 1.2 fallback. Without that
// backend an https URL fails with an error rather than quietly downgrading to
// plaintext, and a certificate that does not verify fails the same way.

namespace rx::http {

struct Header {
  base::String name;
  base::String value;
};

struct RX_HTTP_EXPORT Request {
  base::String method{"GET"};
  base::String url;
  base::Vector<Header> headers;
  base::String body;
  base::String content_type;  // sent only when body is non-empty

  // Armed per socket operation (connect, each read, each write), not on the
  // exchange as a whole: a server that keeps trickling bytes keeps the call
  // alive, a server that stalls does not.
  u32 timeout_ms = 10'000;
  // A response larger than this fails instead of growing the heap. Raise it
  // deliberately for an endpoint known to answer with more.
  u32 max_body_bytes = 4u * 1024 * 1024;
  // 301/302/303 are followed as GET (the browser rule, not the RFC's), 307/308
  // keep the method and body. 0 disables following entirely.
  u32 max_redirects = 4;

  // Trust store override for https. Empty uses the system store; see
  // TlsOptions in stream.h for where that is per platform.
  base::String ca_file;
  // Off only for a host whose certificate cannot be verified (a local test
  // rig). Nothing in a shipping path should set this.
  bool verify_peer = true;
};

struct RX_HTTP_EXPORT Response {
  u16 status = 0;  // 0 means the exchange never produced one; see `error`
  base::Vector<Header> headers;
  base::String body;
  // Why there is no status: DNS, connect, TLS, timeout, a malformed answer.
  // Empty whenever status is non-zero, including for a 500.
  base::String error;

  bool ok() const { return status >= 200 && status < 300; }
  // Case-insensitive header lookup; null when absent. The first match wins,
  // which is what matters for the ones this client acts on (Location,
  // Content-Length, Transfer-Encoding).
  const base::String* Find(const base::String& name) const;
};

// Runs one exchange to completion. Returns a Response with `error` set rather
// than throwing or logging: the caller knows what the request was for and is
// the one that can say what a failure means.
RX_HTTP_EXPORT Response Fetch(const Request& request);

RX_HTTP_EXPORT Response Get(const base::String& url);
RX_HTTP_EXPORT Response PostJson(const base::String& url, const base::String& json);

// Whether this build can speak https at all. False means every https URL comes
// back as an error, which is worth checking once at startup so the operator
// hears about it before the first request.
RX_HTTP_EXPORT bool TlsAvailable();

// --- response parsing, exposed because it is the part worth testing ---

// Splits a response head (status line + headers, no trailing blank line) into
// `out`. False on a malformed status line.
RX_HTTP_EXPORT bool ParseResponseHead(const base::String& head, Response* out);

// Decodes a chunked body. False on a malformed chunk header or when the
// decoded body would pass `max_bytes`.
RX_HTTP_EXPORT bool DecodeChunked(const base::String& raw,
                                  base::String* out,
                                  u32 max_bytes);

}  // namespace rx::http

#endif  // RX_HTTP_HTTP_H_
