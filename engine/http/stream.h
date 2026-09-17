#ifndef RX_HTTP_STREAM_H_
#define RX_HTTP_STREAM_H_

#include <base/strings/xstring.h>

#include <memory>

#include "core/types.h"

namespace rx::http {

// The byte pipe one exchange runs over: a TCP socket, or that socket with a
// TLS record layer on top. Blocking, with the request's timeout armed on the
// socket, so a dead peer fails the call instead of parking the thread.
//
// Module-internal. Fetch() in http.h is the only supported entry point; this
// header exists so the TLS backend can live in its own translation unit.
class Stream {
 public:
  virtual ~Stream() = default;

  // Resolves, connects and (for TLS) completes the handshake. On false the
  // reason is in `error`, phrased for a log line the operator has to act on.
  virtual bool Connect(const base::String& host,
                       u16 port,
                       u32 timeout_ms,
                       base::String* error) = 0;

  // Writes every byte or fails; a short write is retried internally.
  virtual bool Write(const void* data, u32 size, base::String* error) = 0;

  // Bytes read, 0 at a clean end of stream, -1 on error (reason in `error`).
  virtual i64 Read(void* data, u32 size, base::String* error) = 0;
};

// How a TLS stream decides whether to trust the peer.
struct TlsOptions {
  // A PEM bundle to trust instead of the system store. Empty means the system
  // store, which on Linux is the first readable bundle of the usual paths and
  // on Windows is the "ROOT" certificate store.
  base::String ca_file;
  // Off only for reaching a host whose certificate cannot be verified (a local
  // test rig). Every caller that turns this off has to mean it.
  bool verify_peer = true;
};

std::unique_ptr<Stream> MakeTcpStream();

// Null when the build has no TLS backend compiled in (RX_HTTP_TLS off), which
// is what turns an https URL into a clear error instead of a silent downgrade.
std::unique_ptr<Stream> MakeTlsStream(const TlsOptions& options);

}  // namespace rx::http

#endif  // RX_HTTP_STREAM_H_
