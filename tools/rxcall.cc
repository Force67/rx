// rxcall issues one script command to a running engine and prints the reply.
//
//   rxcall <socket> <Command.Name> [args...]
//
// The engine has to be running with --authoring-endpoint <socket>. Argument
// types are inferred from the text: 123 is an int, 1.5 is a float, true/false
// are bools, anything else is a string. That matches what the command schema
// (`rx --dump-commands`) asks for, given that an entity is an int and a vec3 is
// three numbers in a row.
//
// Prints the returned values space-separated on stdout and exits 0, or the
// engine's refusal on stderr and exits 1. Deliberately one call per run: the
// loop this serves is a shell script or an agent, not a human at a prompt.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/containers/vector.h"
#include "base/memory/mem_ops.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "foundation/strings/format.h"
#include "rxe/net/rpc/rpc_message.h"
#include "rxe/net/rpc/rpc_value.h"

namespace net = rx::net;
using rx::u8;
using rx::u32;

namespace {

int Fail(const base::String& message) {
  ::fprintf(stderr, "rxcall: %s\n", message.c_str());
  return 1;
}

net::RpcValue ParseArg(const char* text) {
  if (::strcmp(text, "true") == 0) return net::RpcValue(true);
  if (::strcmp(text, "false") == 0) return net::RpcValue(false);
  char* end = nullptr;
  const long long as_int = ::strtoll(text, &end, 10);
  if (end != text && *end == '\0') return net::RpcValue(static_cast<rx::i64>(as_int));
  const double as_float = ::strtod(text, &end);
  if (end != text && *end == '\0') return net::RpcValue(as_float);
  return net::RpcValue(base::String(text));
}

base::String Format(const net::RpcValue& value) {
  switch (value.type()) {
    case net::RpcValue::Type::kNull: return "null";
    case net::RpcValue::Type::kBool: return value.as_bool() ? "true" : "false";
    case net::RpcValue::Type::kInt: return rx::ToString(value.as_int());
    case net::RpcValue::Type::kFloat: {
      char buffer[64];
      ::snprintf(buffer, sizeof(buffer), "%g", value.as_float());
      return buffer;
    }
    case net::RpcValue::Type::kString: return value.as_string();
    case net::RpcValue::Type::kBlob: return "<blob>";
  }
  return "?";
}

// Reads exactly `count` bytes or fails; a short read here means the engine went
// away mid-reply, which is an error rather than a partial result to print.
bool ReadExactly(int fd, u8* out, size_t count) {
  size_t got = 0;
  while (got < count) {
    const ssize_t chunk = ::read(fd, out + got, count - got);
    if (chunk <= 0) {
      if (chunk < 0 && errno == EINTR) continue;
      return false;
    }
    got += static_cast<size_t>(chunk);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    ::fprintf(stderr, "usage: rxcall <socket> <Command.Name> [args...]\n");
    return 2;
  }
  const base::String socket_path = argv[1];

  net::RpcCall call;
  call.name = argv[2];
  for (int i = 3; i < argc; ++i) call.args.push_back(ParseArg(argv[i]));

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path)) return Fail("socket path is too long");
  base::MemCopy(addr.sun_path, socket_path.c_str(), socket_path.size());

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return Fail(base::String("socket: ") + ::strerror(errno));
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    const base::String reason = ::strerror(errno);
    ::close(fd);
    return Fail("connect " + socket_path + ": " + reason +
                " (is rx running with --authoring-endpoint?)");
  }

  base::Vector<u8> frame;
  const base::Vector<u8> payload = net::EncodeCall(call);
  for (int i = 0; i < 4; ++i)
    frame.push_back(static_cast<u8>(static_cast<u32>(payload.size()) >> (8 * i)));
  frame.insert(frame.end(), payload.begin(), payload.end());
  if (::write(fd, frame.data(), frame.size()) != static_cast<ssize_t>(frame.size())) {
    ::close(fd);
    return Fail("short write");
  }

  u8 header[4];
  if (!ReadExactly(fd, header, sizeof(header))) {
    ::close(fd);
    return Fail("no reply (the engine closed the connection)");
  }
  const u32 length = u32(header[0]) | u32(header[1]) << 8 | u32(header[2]) << 16 |
                     u32(header[3]) << 24;
  base::Vector<u8> reply_bytes(length);
  if (length && !ReadExactly(fd, reply_bytes.data(), length)) {
    ::close(fd);
    return Fail("truncated reply");
  }
  ::close(fd);

  base::Optional<net::RpcCall> reply = net::DecodeCall(reply_bytes.data(), reply_bytes.size());
  if (!reply) return Fail("malformed reply");
  if (reply->name != "ok") {
    return Fail(reply->args.empty() ? "refused" : Format(reply->args[0]));
  }

  base::String out;
  for (const net::RpcValue& value : reply->args) {
    if (!out.empty()) out += ' ';
    out += Format(value);
  }
  ::printf("%s\n", out.c_str());
  return 0;
}
