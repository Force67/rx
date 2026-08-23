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
// and the rpc wire vocabulary (rpc/rpc_value.h), so an out-of-process tool can
// drive a running engine. A decoded RpcCall is checked against the command's
// HandlerSig, marshalled into a ScriptValue stack, dispatched, and the handler's
// return marshalled back out.
//
// THREAT MODEL. These commands spawn, destroy, move and rename arbitrary
// entities: reaching them is equivalent to owning the running scene. Two gates
// stand in front of them.
//
//  1. The bridge owns its OWN RpcRegistry, which is never the net session's
//     (net/rpc_channel.h owns that one). An authoring name therefore does not
//     resolve at all on the game's packet path: a hostile client cannot reach
//     World.Destroy by sending it, because nothing the net layer dispatches
//     through knows that name.
//  2. rpc::RpcContext cannot express "this came from a trusted local origin" --
//     it carries a peer id and a from_server flag, and a remote packet produces
//     both. So trust is established by the TRANSPORT (CommandEndpoint checks the
//     connecting process's uid) and carried in `sender` as kLocalSender, which
//     is zetanet's invalid peer id and so is never assigned to a connected
//     client. Invoke refuses every other context, which keeps the gate shut even
//     if a later app hands this registry to a net channel by mistake.
//
// What anyone past both gates can do: every registered command, with no
// per-command permission. That is deliberate (the endpoint exists for a trusted
// authoring agent on the same machine) and is why nothing starts it by default.
//
// Marshalling rules, driven entirely by HandlerSig so they cannot drift from the
// handlers:
//   bool/int/float/string  one rpc value each (an int is widened where a float
//                          is expected; nothing else converts)
//   symbol                 an rpc string, interned through the context
//   entity                 an rpc int, index in the low 32 bits, generation in
//                          the high 32
//   vec3                   THREE numeric rpc values, so a wire call has more
//                          args than the signature has params
// A call whose args do not match is refused with a message naming the mismatch,
// never reinterpreted.

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
