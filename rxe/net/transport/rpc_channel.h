#ifndef RX_NET_RPC_CHANNEL_H_
#define RX_NET_RPC_CHANNEL_H_

// Compiled only when the transport is available (RX_NET_HAS_ZETANET).

#include <stddef.h>

#include <znet/z_client.h>
#include <znet/z_server.h>

#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"
#include "rxe/net/rpc/rpc_message.h"
#include "rxe/net/rpc/rpc_registry.h"

namespace rx::net {

// Carries scripting RPCs over the session's reliable data channel and
// dispatches inbound calls through a registry the game populates. The server
// channel attributes each call to the peer that sent it and can target a
// specific client or broadcast; the client channel talks only to the host.
// Both own their registry so the scripting runtime registers handlers in one
// place.

class RX_NET_EXPORT RpcServerChannel {
 public:
  explicit RpcServerChannel(tx::network::ZServer& server) : server_(server) {}

  rpc::RpcRegistry& registry() { return registry_; }

  // Decodes a kRpcCall from `peer` and dispatches it with that peer as the
  // sender. Drops a malformed call or one with no registered handler.
  void OnPacket(u32 peer, const u8* data, size_t size);

  // Sends a call to one client. Returns false if the encoded call exceeds the
  // single-datagram limit (the caller should split its payload).
  bool EmitToClient(u32 peer, const rpc::RpcCall& call);
  bool EmitToClient(u32 peer, base::String name, rpc::RpcArgs args);

  // Sends a call to every connected client.
  bool Broadcast(const rpc::RpcCall& call);
  bool Broadcast(base::String name, rpc::RpcArgs args);

 private:
  bool Send(u32 destination, const rpc::RpcCall& call);

  tx::network::ZServer& server_;
  rpc::RpcRegistry registry_;
};

class RX_NET_EXPORT RpcClientChannel {
 public:
  explicit RpcClientChannel(tx::network::ZClient& client) : client_(client) {}

  rpc::RpcRegistry& registry() { return registry_; }

  // Decodes a kRpcCall from the host and dispatches it as a server-originated
  // call. Drops a malformed call or one with no registered handler.
  void OnPacket(const u8* data, size_t size);

  // Sends a call to the host. Returns false if the encoded call exceeds the
  // single-datagram limit.
  bool EmitToServer(const rpc::RpcCall& call);
  bool EmitToServer(base::String name, rpc::RpcArgs args);

 private:
  tx::network::ZClient& client_;
  rpc::RpcRegistry registry_;
};

}  // namespace rx::net

#endif  // RX_NET_RPC_CHANNEL_H_
