#ifndef RX_AUTHORING_COMMAND_BRIDGE_H_
#define RX_AUTHORING_COMMAND_BRIDGE_H_

#include <string>

#include "core/export.h"
#include "core/types.h"
#include "rpc/rpc_message.h"
#include "rpc/rpc_registry.h"
#include "rpc/rpc_value.h"

namespace rx::script {
struct HandlerContext;
class HandlerRegistry;
}  // namespace rx::script

namespace rx::authoring {

// The seam between the typed script command registry (script/handler_registry.h)
// and the rpc wire vocabulary (rpc/rpc_value.h): decodes an RpcCall against the
// command's HandlerSig, marshals a ScriptValue stack, dispatches, marshals the
// return back out.
//
// THREAT MODEL. These commands spawn, destroy, move and rename arbitrary
// entities: reaching them equals owning the running scene. Two gates:
//  1. The bridge owns its OWN RpcRegistry, never the net session's, so an
//     authoring name does not resolve on the game's packet path at all.
//  2. rpc::RpcContext cannot express "trusted local origin" (a peer id plus
//     from_server is producible by a remote packet), so trust is established by
//     the TRANSPORT (CommandEndpoint checks the connecting uid) and carried in
//     `sender` as kLocalSender, zetanet's never-assigned invalid peer id.
//     Invoke refuses every other context.
// Past both gates: every registered command, no per-command permissions. That
// is deliberate (the endpoint serves a trusted local authoring agent) and why
// nothing starts it by default.
//
// Marshalling is driven entirely by HandlerSig so it cannot drift:
// bool/int/float/string one rpc value each (int widens to float, nothing else
// converts); symbol an interned rpc string; entity an rpc int (index low 32,
// generation high 32); vec3 THREE numeric values, so a wire call has more args
// than the signature has params. A mismatched call is refused naming the
// mismatch, never reinterpreted.

// The sender id CommandEndpoint stamps on a call that cleared its uid check.
// Equals tx::network::ZPeerId::invalid_id, so no connected peer can present it.
inline constexpr u32 kLocalSender = 0xffffffffu;

class RX_AUTHORING_EXPORT CommandBridge {
 public:
  // Registers every command currently in `commands` under its own name in the
  // bridge's rpc registry. `commands` and `ctx` must outlive the bridge, and a
  // command added after construction is not exposed (registration completes at
  // engine start, before anything can call in).
  CommandBridge(script::HandlerRegistry& commands, script::HandlerContext& ctx);

  // Names known to the bridge, for a transport that wants to reject early.
  const rpc::RpcRegistry& registry() const { return registry_; }

  struct Reply {
    bool ok = false;
    // The handler's return, flattened (a vec3 return is three floats). Empty for
    // a void command. Strings are copied out of the context's scratch arena, so
    // the reply stays valid after the arena is reset.
    rpc::RpcArgs values;
    std::string error;  // set only when !ok, safe to show a caller
  };

  // Checks, marshals, dispatches. Never throws and never asserts on caller data:
  // an unknown name, an untrusted context or a signature mismatch all come back
  // as ok=false. Synchronous, so the caller may pass args that only live for the
  // duration of the call (string args are borrowed, not copied).
  Reply Invoke(const rpc::RpcContext& ctx, const rpc::RpcCall& call);

 private:
  script::HandlerRegistry& commands_;
  script::HandlerContext& ctx_;
  rpc::RpcRegistry registry_;
  // Where the registered rpc handler drops what the script handler returned.
  // RpcHandler returns void, so a request/reply endpoint has to catch the result
  // on the side; this is safe because Dispatch is synchronous and single
  // threaded, and the pointer is only non-null inside Invoke.
  Reply* pending_ = nullptr;
};

}  // namespace rx::authoring

#endif  // RX_AUTHORING_COMMAND_BRIDGE_H_
