#include "authoring/command_endpoint.h"

#include "authoring/command_bridge.h"
#include "core/log.h"
#include "rpc/rpc_message.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <utility>
#endif

namespace rx::authoring {
namespace {

// An authoring call is a name and a handful of scalars; anything approaching
// this is a bug or an attack, and is dropped with the connection rather than
// allocated.
constexpr u32 kMaxFrame = 1u << 20;
// Enough for an agent plus a human poking at it. A bound at all is what stops a
// local process from parking the engine's whole fd budget on this socket.
constexpr size_t kMaxClients = 8;

#if !defined(_WIN32)
#if defined(MSG_NOSIGNAL)
// A client that exits between sending its call and reading the reply is normal
// (a shell loop with a ctrl-c in it). Without this the write raises SIGPIPE and
// the default disposition takes the whole engine down with it.
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // apple: SO_NOSIGPIPE is set on the socket instead
#endif
#endif

}  // namespace

#if defined(_WIN32)

// AF_UNIX exists on modern Windows but through winsock, which this file does not
// speak. The endpoint is a developer/agent tool, so it simply is not offered
// there rather than growing a second transport.
CommandEndpoint::~CommandEndpoint() = default;

bool CommandEndpoint::Start(const std::string& path, std::string* error) {
  (void)path;
  if (error) *error = "the authoring endpoint needs a posix unix socket";
  return false;
}

void CommandEndpoint::Poll(CommandBridge& bridge) { (void)bridge; }
void CommandEndpoint::Stop() {}

#else

namespace {

bool SetNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
  // The engine spawns processes (asset tooling); an inherited listener would
  // keep the socket alive past our own exit.
  return ::fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0;
}

// True when something is already serving `path`. A socket file outliving its
// process is the normal case after a crash and is safe to unlink; a live one is
// not, and taking it over would silently steal another engine's endpoint.
bool SocketIsLive(const sockaddr_un& addr) {
  const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (probe < 0) return false;
  const bool live = ::connect(probe, reinterpret_cast<const sockaddr*>(&addr),
                              sizeof(addr)) == 0;
  ::close(probe);
  return live;
}

// The uid on the far end of an accepted connection. False when the platform
// cannot answer, which is treated as untrusted.
bool PeerUid(int fd, uid_t* uid) {
#if defined(__linux__)
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
  *uid = cred.uid;
  return true;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  gid_t gid = 0;
  if (::getpeereid(fd, uid, &gid) != 0) return false;
  return true;
#else
  (void)fd;
  (void)uid;
  return false;
#endif
}

void PutU32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

u32 ReadU32(const u8* p) {
  return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}

}  // namespace

CommandEndpoint::~CommandEndpoint() { Stop(); }

bool CommandEndpoint::Start(const std::string& path, std::string* error) {
  auto fail = [&](std::string message) {
    if (error) *error = std::move(message);
    if (listener_ >= 0) {
      ::close(listener_);
      listener_ = -1;
    }
    return false;
  };

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    return fail("socket path is longer than " +
                std::to_string(sizeof(addr.sun_path) - 1) + " bytes");
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  if (SocketIsLive(addr)) return fail("another process is already serving " + path);
  ::unlink(path.c_str());

  listener_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener_ < 0) return fail(std::string("socket: ") + std::strerror(errno));
  if (!SetNonBlocking(listener_)) return fail(std::string("fcntl: ") + std::strerror(errno));
  if (::bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    return fail(std::string("bind: ") + std::strerror(errno));
  // The uid check at accept is the real gate; the mode keeps a call from another
  // account from even reaching it.
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0)
    return fail(std::string("chmod: ") + std::strerror(errno));
  if (::listen(listener_, 4) != 0) return fail(std::string("listen: ") + std::strerror(errno));

  path_ = path;
  return true;
}

void CommandEndpoint::Stop() {
  for (int fd : clients_) ::close(fd);
  clients_.clear();
  inbox_.clear();
  if (listener_ >= 0) {
    ::close(listener_);
    listener_ = -1;
  }
  if (!path_.empty()) {
    ::unlink(path_.c_str());
    path_.clear();
  }
}

void CommandEndpoint::Accept() {
  for (;;) {
    const int fd = ::accept(listener_, nullptr, nullptr);
    if (fd < 0) return;  // EAGAIN: nothing more pending
    uid_t peer = 0;
    if (!PeerUid(fd, &peer)) {
      RX_WARN("authoring: refused a connection whose peer uid could not be read");
      ::close(fd);
      continue;
    }
    if (peer != ::geteuid()) {
      RX_WARN("authoring: refused a connection from uid {}", static_cast<u32>(peer));
      ::close(fd);
      continue;
    }
    if (clients_.size() >= kMaxClients || !SetNonBlocking(fd)) {
      ::close(fd);
      continue;
    }
#if defined(SO_NOSIGPIPE)
    const int on = 1;  // the apple spelling of MSG_NOSIGNAL, see kSendFlags
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    clients_.push_back(fd);
    inbox_.emplace_back();
  }
}

bool CommandEndpoint::Serve(int index, CommandBridge& bridge) {
  const int fd = clients_[static_cast<size_t>(index)];
  std::vector<u8>& buffer = inbox_[static_cast<size_t>(index)];

  u8 chunk[4096];
  for (;;) {
    const ssize_t got = ::read(fd, chunk, sizeof(chunk));
    if (got > 0) {
      if (buffer.size() + static_cast<size_t>(got) > kMaxFrame + 4) return false;
      buffer.insert(buffer.end(), chunk, chunk + got);
      continue;
    }
    if (got == 0) return false;  // peer closed
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    return false;
  }

  size_t consumed = 0;
  while (buffer.size() - consumed >= 4) {
    const u32 length = ReadU32(buffer.data() + consumed);
    if (length > kMaxFrame) {
      RX_WARN("authoring: dropping a client that framed {} bytes", length);
      return false;
    }
    if (buffer.size() - consumed - 4 < length) break;  // partial, wait for more
    const u8* payload = buffer.data() + consumed + 4;

    rpc::RpcCall reply;
    std::optional<rpc::RpcCall> call = rpc::DecodeCall(payload, length);
    if (!call) {
      reply.name = "error";
      reply.args.emplace_back(std::string("malformed rpc frame"));
    } else {
      // The uid check at accept is what makes this a trusted origin; the sender
      // id is how that fact reaches the bridge (see kLocalSender).
      const rpc::RpcContext ctx{kLocalSender, /*from_server=*/false};
      CommandBridge::Reply result = bridge.Invoke(ctx, *call);
      reply.name = result.ok ? "ok" : "error";
      if (result.ok)
        reply.args = std::move(result.values);
      else
        reply.args.emplace_back(std::move(result.error));
    }
    consumed += 4 + length;

    std::vector<u8> frame;
    const std::vector<u8> encoded = rpc::EncodeCall(reply);
    PutU32(frame, static_cast<u32>(encoded.size()));
    frame.insert(frame.end(), encoded.begin(), encoded.end());
    // A reply is tens of bytes against a socket buffer of hundreds of kilobytes,
    // so a short write means the peer is gone or wedged; dropping it is the
    // honest outcome, and beats growing an outbound queue nobody needs.
    const ssize_t wrote = ::send(fd, frame.data(), frame.size(), kSendFlags);
    if (wrote < 0 || static_cast<size_t>(wrote) != frame.size()) return false;
  }

  buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(consumed));
  return true;
}

void CommandEndpoint::Poll(CommandBridge& bridge) {
  if (listener_ < 0) return;
  Accept();
  for (size_t i = clients_.size(); i-- > 0;) {
    if (Serve(static_cast<int>(i), bridge)) continue;
    ::close(clients_[i]);
    clients_.erase(clients_.begin() + static_cast<ptrdiff_t>(i));
    inbox_.erase(inbox_.begin() + static_cast<ptrdiff_t>(i));
  }
}

#endif  // !_WIN32

}  // namespace rx::authoring
