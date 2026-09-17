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

#include <cstdio>
#include <cstring>
#include <mutex>

namespace rx::http {
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
  WSAPOLLFD fd{};
  fd.fd = s;
  fd.events = POLLWRNORM;
  return ::WSAPoll(&fd, 1, timeout_ms);
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

// Winsock needs one process-wide startup before any socket call, and nothing
// else in rx guarantees it has happened (zetanet does its own, but rx::http
// builds without zetanet).
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

base::String Describe(const char* what, int err) {
  char buffer[192] = {};
  std::snprintf(buffer, sizeof(buffer), "%s: %s (%d)", what, std::strerror(err), err);
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
               u32 timeout_ms,
               base::String* error) override {
    EnsureSocketLibrary();

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
                    ::gai_strerror(rc));
      *error = base::String(buffer);
      return false;
    }

    // Every address the resolver returned, in its order: a host with a AAAA
    // record on a v4-only network is the case that makes this a loop.
    base::String last;
    for (addrinfo* ai = resolved; ai != nullptr; ai = ai->ai_next) {
      if (TryConnect(*ai, timeout_ms, &last)) {
        ::freeaddrinfo(resolved);
        SetTimeouts(socket_, timeout_ms);
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
      const auto sent = ::send(socket_, cursor, static_cast<int>(left), kSendFlags);
      if (sent > 0) {
        cursor += sent;
        left -= static_cast<u32>(sent);
        continue;
      }
      const int err = LastError();
      if (ErrorIsInterrupt(err))
        continue;
      *error = ErrorIsTimeout(err) ? base::String("write timed out")
                                   : Describe("write failed", err);
      return false;
    }
    return true;
  }

  i64 Read(void* data, u32 size, base::String* error) override {
    for (;;) {
      const auto got = ::recv(socket_, static_cast<char*>(data), static_cast<int>(size), 0);
      if (got >= 0)
        return static_cast<i64>(got);
      const int err = LastError();
      if (ErrorIsInterrupt(err))
        continue;
      *error = ErrorIsTimeout(err) ? base::String("read timed out")
                                   : Describe("read failed", err);
      return -1;
    }
  }

 private:
  bool TryConnect(const addrinfo& ai, u32 timeout_ms, base::String* last_error) {
    SocketHandle sock = ::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol);
    if (sock == kInvalidSocket) {
      *last_error = Describe("socket", LastError());
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
        *last_error = Describe("connect", err);
        CloseSocket(sock);
        return false;
      }
      const int ready = PollWritable(sock, static_cast<int>(timeout_ms));
      if (ready <= 0) {
        *last_error = ready == 0 ? base::String("connect timed out")
                                 : Describe("connect poll", LastError());
        CloseSocket(sock);
        return false;
      }
      // Writable also means "failed": the verdict is in SO_ERROR.
      int so_error = 0;
      SockLen len = sizeof(so_error);
      if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) !=
              0 ||
          so_error != 0) {
        *last_error = Describe("connect", so_error != 0 ? so_error : LastError());
        CloseSocket(sock);
        return false;
      }
    }

    SetNonBlocking(sock, false);
    socket_ = sock;
    return true;
  }

  SocketHandle socket_ = kInvalidSocket;
};

}  // namespace

std::unique_ptr<Stream> MakeTcpStream() {
  return std::make_unique<TcpStream>();
}

}  // namespace rx::http
