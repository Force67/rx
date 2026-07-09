#ifndef RX_RPC_RPC_REGISTRY_H_
#define RX_RPC_RPC_REGISTRY_H_

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/types.h"
#include "rpc/rpc_message.h"
#include "rpc/rpc_value.h"

namespace rx::rpc {

// Identifies who sent an RPC, so a handler can authorize or attribute it. On the
// server, sender is the zetanet peer id of the calling client. On the client,
// from_server is true and sender is 0 (the call came from the host).
struct RpcContext {
  u32 sender = 0;
  bool from_server = false;
};

using RpcHandler = std::function<void(const RpcContext&, const RpcArgs&)>;

// Maps RPC names to handlers and dispatches decoded calls to them. The net layer
// decodes incoming bytes into an RpcCall, builds the RpcContext from the peer it
// arrived on, and calls Dispatch. A call for an unregistered name is dropped, not
// an error, because a peer may send names this build does not implement.
class RpcRegistry {
 public:
  void On(std::string name, RpcHandler handler);  // registers or replaces
  bool Has(std::string_view name) const;

  // Looks up the call's name and invokes its handler. Returns false (and does
  // nothing) when no handler is registered; the caller logs and drops. Never
  // throws on a missing handler.
  bool Dispatch(const RpcContext& ctx, const RpcCall& call) const;

  void Clear();
  size_t size() const;

 private:
  std::unordered_map<std::string, RpcHandler> handlers_;
};

}  // namespace rx::rpc

#endif  // RX_RPC_RPC_REGISTRY_H_
