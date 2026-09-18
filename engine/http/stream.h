#ifndef RX_HTTP_STREAM_H_
#define RX_HTTP_STREAM_H_

#include <base/strings/xstring.h>

#include <atomic>
#include <chrono>
#include <memory>

#include "core/types.h"

namespace rx::http {

// What bounds one exchange in time. The stream enforces all three, because it
// is the layer that actually waits: the socket is armed with a short tick and
// the read loop re-checks these on every wakeup, so a cancel lands in about a
// quarter second instead of whenever the peer next says something.
struct StreamLimits {
  // Idle timeout: how long the peer may say NOTHING before the exchange fails.
  // A server that keeps sending keeps the call alive; a stalled one does not.
  u32 idle_ms = 10'000;
  // Wall-clock end of the whole exchange, connect and body included. Default
  // constructed (the epoch) means no overall bound, only the idle timeout.
  std::chrono::steady_clock::time_point deadline{};
  // Raised by the caller to abandon the exchange. Owned by the caller, read
  // from this thread, so it has to outlive the call.
  const std::atomic<bool>* cancel = nullptr;

  bool has_deadline() const {
    return deadline != std::chrono::steady_clock::time_point{};
  }
  bool cancelled() const {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
  }
  // Milliseconds left before the deadline, or -1 when there is none. 0 means
  // it has passed.
  i64 remaining_ms() const;
};

// The byte pipe one exchange runs over: a TCP socket, or that socket with a
// TLS record layer on top. Blocking, bounded by the limits above.
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
                       const StreamLimits& limits,
                       base::String* error) = 0;

  // Writes every byte or fails; a short write is retried internally.
  virtual bool Write(const void* data, u32 size, base::String* error) = 0;

  // Bytes read, 0 at a clean end of stream, -1 on error (reason in `error`).
  virtual i64 Read(void* data, u32 size, base::String* error) = 0;

  // True when the last failure was the caller's own cancel rather than
  // anything the peer or the network did. The distinction matters: a caller
  // that shut down on purpose must not log that as a fault.
  virtual bool was_cancelled() const = 0;
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
