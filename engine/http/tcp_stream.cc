#include "http/stream.h"

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace rx::http {

i64 StreamLimits::remaining_ms() const {
  if (!has_deadline())
    return -1;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
  return left > 0 ? static_cast<i64>(left) : 0;
}

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SockLen = int;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;

void CloseSocket(SocketHandle s) {
  ::closesocket(s);
}
int LastError() {
  return ::WSAGetLastError();
}
bool ErrorIsTimeout(int err) {
  return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK;
}
bool ErrorIsInProgress(int err) {
  return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
bool ErrorIsInterrupt(int) {
  return false;  // Winsock has no EINTR on these calls.
}
int PollWritable(SocketHandle s, int timeout_ms) {
  // Not WSAPoll: it is documented not to report a connection attempt that
  // FAILED, so a refused connect would wait out the whole timeout and then be
  // reported as one. select's exception set is how Windows says "refused".
  fd_set write_set;
  fd_set error_set;
  FD_ZERO(&write_set);
  FD_ZERO(&error_set);
  FD_SET(s, &write_set);
  FD_SET(s, &error_set);
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  return ::select(0, nullptr, &write_set, &error_set, &tv);
}
void SetNonBlocking(SocketHandle s, bool on) {
  u_long mode = on ? 1 : 0;
  ::ioctlsocket(s, FIONBIO, &mode);
}
void SetTimeouts(SocketHandle s, u32 timeout_ms) {
  const DWORD ms = timeout_ms;
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// Winsock needs one process-wide startup before any socket call. Nothing else
// in rx guarantees it ran: zetanet does its own, and rx::http builds without
// zetanet.
void EnsureSocketLibrary() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    ::WSAStartup(MAKEWORD(2, 2), &data);
  });
}
#else
using SocketHandle = int;
using SockLen = socklen_t;
constexpr SocketHandle kInvalidSocket = -1;
// Never raise SIGPIPE: a peer that hangs up mid-write is an error code here,
// not a signal that takes the process down. macOS has no MSG_NOSIGNAL and
// spells it as the SO_NOSIGPIPE sockopt set in TryConnect instead.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void CloseSocket(SocketHandle s) {
  ::close(s);
}
int LastError() {
  return errno;
}
bool ErrorIsTimeout(int err) {
  return err == EAGAIN || err == EWOULDBLOCK;
}
bool ErrorIsInProgress(int err) {
  return err == EINPROGRESS;
}
bool ErrorIsInterrupt(int err) {
  return err == EINTR;
}
int PollWritable(SocketHandle s, int timeout_ms) {
  pollfd fd{};
  fd.fd = s;
  fd.events = POLLOUT;
  return ::poll(&fd, 1, timeout_ms);
}
void SetNonBlocking(SocketHandle s, bool on) {
  int flags = ::fcntl(s, F_GETFL, 0);
  if (flags < 0)
    return;
  flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  ::fcntl(s, F_SETFL, flags);
}
void SetTimeouts(SocketHandle s, u32 timeout_ms) {
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout_ms / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
void EnsureSocketLibrary() {}
#endif

base::String SocketError(const char* what, int err) {
  char buffer[320] = {};
#ifdef _WIN32
  // WSA error codes start at 10000 and are not errno values: strerror answered
  // "Unknown error 10061" where the system had "Connection refused".
  char text[192] = {};
  const DWORD written = ::FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
      static_cast<DWORD>(err), 0, text, sizeof(text) - 1, nullptr);
  for (DWORD i = 0; i < written; ++i) {
    if (text[i] == '\r' || text[i] == '\n')
      text[i] = ' ';
  }
  std::snprintf(buffer, sizeof(buffer), "%s: %s (%d)", what,
                written != 0 ? text : "socket error", err);
#else
  std::snprintf(buffer, sizeof(buffer), "%s: %s (%d)", what, std::strerror(err), err);
#endif
  return base::String(buffer);
}

class TcpStream final : public Stream {
 public:
  ~TcpStream() override {
    if (socket_ != kInvalidSocket)
      CloseSocket(socket_);
  }

  bool Connect(const base::String& host,
               u16 port,
               const StreamLimits& limits,
               base::String* error) override {
    EnsureSocketLibrary();
    limits_ = limits;
    if (Stopped(error))
      return false;

    char service[8] = {};
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* resolved = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), service, &hints, &resolved);
    if (rc != 0 || resolved == nullptr) {
      char buffer[256] = {};
      std::snprintf(buffer, sizeof(buffer), "cannot resolve %s: %s", host.c_str(),
                    rc != 0 ? ::gai_strerror(rc) : "the resolver returned no addresses");
      *error = base::String(buffer);
      if (resolved != nullptr)
        ::freeaddrinfo(resolved);
      return false;
    }

    // Every address the resolver returned, in its order: a host with a AAAA
    // record on a v4-only network is the case that makes this a loop.
    base::String last;
    for (addrinfo* ai = resolved; ai != nullptr; ai = ai->ai_next) {
      if (Stopped(error)) {
        ::freeaddrinfo(resolved);
        return false;
      }
      if (TryConnect(*ai, ConnectBudgetMs(), &last)) {
        ::freeaddrinfo(resolved);
        // The socket wakes on a short tick rather than the idle timeout, so
        // the loops below can notice a cancel or the deadline between reads
        // instead of only when the peer says something.
        SetTimeouts(socket_, kWakeTickMs);
        MarkProgress();
        return true;
      }
    }
    ::freeaddrinfo(resolved);
    char buffer[320] = {};
    std::snprintf(buffer, sizeof(buffer), "cannot connect to %s:%u (%s)", host.c_str(),
                  static_cast<unsigned>(port), last.c_str());
    *error = base::String(buffer);
    return false;
  }

  bool Write(const void* data, u32 size, base::String* error) override {
    const char* cursor = static_cast<const char*>(data);
    u32 left = size;
    while (left > 0) {
      if (Stopped(error))
        return false;
      const auto sent = ::send(socket_, cursor, static_cast<int>(left), kSendFlags);
      if (sent > 0) {
        cursor += sent;
        left -= static_cast<u32>(sent);
        MarkProgress();
        continue;
      }
      const int err = LastError();
      if (ErrorIsInterrupt(err))
        continue;
      // The socket's own timeout is the wake tick, not the idle limit, so a
      // timeout here only means "nothing moved this tick": loop, and let
      // Stopped decide whether the exchange is actually over.
      if (ErrorIsTimeout(err)) {
        if (Idle() > limits_.idle_ms) {
          *error = "write timed out";
          return false;
        }
        continue;
      }
      *error = SocketError("write failed", err);
      return false;
    }
    return true;
  }

  i64 Read(void* data, u32 size, base::String* error) override {
    for (;;) {
      if (Stopped(error))
        return -1;
      const auto got = ::recv(socket_, static_cast<char*>(data), static_cast<int>(size), 0);
      if (got > 0) {
        MarkProgress();
        return static_cast<i64>(got);
      }
      if (got == 0)
        return 0;  // a clean end of stream
      const int err = LastError();
      if (ErrorIsInterrupt(err))
        continue;
      if (ErrorIsTimeout(err)) {
        if (Idle() > limits_.idle_ms) {
          *error = "read timed out";
          return -1;
        }
        continue;
      }
      *error = SocketError("read failed", err);
      return -1;
    }
  }

  bool was_cancelled() const override { return cancelled_; }

 private:
  // How often a blocked socket call wakes to look at the cancel flag and the
  // deadline. Short enough that quitting the game feels immediate, long enough
  // that an idle connection costs nothing to hold.
  static constexpr u32 kWakeTickMs = 250;

  // True when the exchange is over for a reason that is not the peer's: the
  // caller cancelled, or the deadline passed.
  bool Stopped(base::String* error) {
    if (limits_.cancelled()) {
      cancelled_ = true;
      *error = "the request was cancelled";
      return true;
    }
    if (limits_.has_deadline() && limits_.remaining_ms() <= 0) {
      *error = "the request ran out of time";
      return true;
    }
    return false;
  }

  void MarkProgress() { last_progress_ = std::chrono::steady_clock::now(); }

  u32 Idle() const {
    const auto since = std::chrono::steady_clock::now() - last_progress_;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
    return ms < 0 ? 0u : static_cast<u32>(ms);
  }

  // What the connect itself may spend: the idle budget, cut short by the
  // deadline when there is one.
  u32 ConnectBudgetMs() const {
    u32 budget = limits_.idle_ms;
    if (limits_.has_deadline()) {
      const i64 left = limits_.remaining_ms();
      if (left >= 0 && static_cast<u64>(left) < budget)
        budget = static_cast<u32>(left);
    }
    // A cancel has to be noticed during a connect too, so the poll waits in
    // ticks rather than in one long block.
    return budget < kWakeTickMs ? budget : kWakeTickMs;
  }

  bool TryConnect(const addrinfo& ai, u32 tick_ms, base::String* last_error) {
    SocketHandle sock = ::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol);
    if (sock == kInvalidSocket) {
      *last_error = SocketError("socket", LastError());
      return false;
    }
#if defined(SO_NOSIGPIPE)
    const int on = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif

    // Non-blocking for the connect alone, so the attempt is bounded by the
    // request timeout rather than by the kernel's SYN retry schedule.
    SetNonBlocking(sock, true);
    int rc = ::connect(sock, ai.ai_addr, static_cast<int>(ai.ai_addrlen));
    if (rc != 0) {
      const int err = LastError();
      if (!ErrorIsInProgress(err)) {
        *last_error = SocketError("connect", err);
        CloseSocket(sock);
        return false;
      }
      // A signal interrupts poll without restarting it (SA_RESTART does not
      // cover it), so a process with a periodic timer would see a spurious
      // "connect failed". Wait in ticks instead, which also gives the cancel
      // flag and the deadline a look in between.
      const auto give_up =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(limits_.idle_ms);
      int ready = 0;
      for (;;) {
        ready = PollWritable(sock, static_cast<int>(tick_ms));
        if (ready > 0)
          break;
        if (ready < 0 && !ErrorIsInterrupt(LastError()))
          break;
        base::String stop;
        if (Stopped(&stop)) {
          *last_error = stop;
          CloseSocket(sock);
          return false;
        }
        if (std::chrono::steady_clock::now() >= give_up) {
          ready = 0;
          break;
        }
      }
      if (ready <= 0) {
        *last_error = ready == 0 ? base::String("connect timed out")
                                 : SocketError("connect poll", LastError());
        CloseSocket(sock);
        return false;
      }
      // Writable also means "failed": the verdict is in SO_ERROR.
      int so_error = 0;
      SockLen len = sizeof(so_error);
      if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) !=
              0 ||
          so_error != 0) {
        *last_error = SocketError("connect", so_error != 0 ? so_error : LastError());
        CloseSocket(sock);
        return false;
      }
    }

    SetNonBlocking(sock, false);
    socket_ = sock;
    return true;
  }

  SocketHandle socket_ = kInvalidSocket;
  StreamLimits limits_;
  std::chrono::steady_clock::time_point last_progress_ = std::chrono::steady_clock::now();
  bool cancelled_ = false;
};

}  // namespace

std::unique_ptr<Stream> MakeTcpStream() {
  return std::make_unique<TcpStream>();
}

}  // namespace rx::http
