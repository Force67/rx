#ifndef RX_RPC_RPC_REGISTRY_H_
#define RX_RPC_RPC_REGISTRY_H_

#include "base/containers/unordered_map.h"
#include "base/functional/function.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"
#include "rxe/rpc/rpc_message.h"
#include "rxe/rpc/rpc_value.h"

namespace rx::rpc {

// Identifies who sent an RPC, so a handler can authorize or attribute it. On the
// server, sender is the zetanet peer id of the calling client. On the client,
// from_server is true and sender is 0 (the call came from the host).
struct RpcContext {
  u32 sender = 0;
  bool from_server = false;
};

using RpcHandler = base::Function<void(const RpcContext&, const RpcArgs&)>;

// Maps RPC names to handlers and dispatches decoded calls to them. The net layer
// decodes incoming bytes into an RpcCall, builds the RpcContext from the peer it
// arrived on, and calls Dispatch. A call for an unregistered name is dropped, not
// an error, because a peer may send names this build does not implement.
class RX_RPC_EXPORT RpcRegistry {
 public:
  void On(base::String name, RpcHandler handler);  // registers or replaces
  bool Has(base::StringRef name) const;

  // Looks up the call's name and invokes its handler. Returns false (and does
  // nothing) when no handler is registered; the caller logs and drops. Never
  // throws on a missing handler.
  bool Dispatch(const RpcContext& ctx, const RpcCall& call) const;

  void Clear();
  size_t size() const;

 private:
  base::UnorderedMap<base::String, RpcHandler> handlers_;
};

}  // namespace rx::rpc

#endif  // RX_RPC_RPC_REGISTRY_H_
