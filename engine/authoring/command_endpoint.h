#ifndef RX_AUTHORING_COMMAND_ENDPOINT_H_
#define RX_AUTHORING_COMMAND_ENDPOINT_H_

#include <string>
#include <vector>

#include "core/export.h"
#include "core/types.h"

namespace rx::authoring {

class CommandBridge;

// A unix domain socket that lets an out-of-process tool call the engine's script
// commands while it runs. The frame is a u32 little-endian length followed by
// exactly that many bytes of rpc::EncodeCall output; the reply is one frame in
// the same shape, an RpcCall named "ok" whose args are the command's return, or
// "error" whose single arg is the message.
//
// Trust is the transport's job (see the threat model on CommandBridge): the
// socket file is created 0600 and every accepted connection has to belong to the
// same uid as this process, so "local and trusted" means the user already able
// to attach a debugger to the engine. There is no authentication beyond that,
// which is why the app has to opt in by calling Start with a path.
//
// Polled, never threaded: Poll runs on the caller's thread and dispatches inline,
// so a command mutates the ECS on the same thread the simulation does. The cost
// is a reply latency of up to one frame, which is nothing against the round trip
// of a tool that spawns a process per call.
class RX_AUTHORING_EXPORT CommandEndpoint {
 public:
  ~CommandEndpoint();

  // Binds and listens on `path`, replacing a stale socket file left by a crashed
  // run. False + *error on any failure (a path in a missing directory, a path
  // already served by a live process, an unsupported platform).
  bool Start(const std::string& path, std::string* error);

  // Accepts new connections and serves every complete frame that has arrived,
  // then returns. Nonblocking: a caller mid-frame is left buffered for the next
  // poll. Does nothing when the endpoint was never started.
  void Poll(CommandBridge& bridge);

  void Stop();  // closes the listener and unlinks the socket file
  bool running() const { return listener_ >= 0; }

 private:
  void Accept();
  // Serves what `client` has sent. False when the connection should be dropped
  // (peer closed, protocol violation, write failure).
  bool Serve(int index, CommandBridge& bridge);

  int listener_ = -1;
  std::string path_;
  std::vector<int> clients_;
  std::vector<std::vector<u8>> inbox_;  // per client, parallel to clients_
};

}  // namespace rx::authoring

#endif  // RX_AUTHORING_COMMAND_ENDPOINT_H_
