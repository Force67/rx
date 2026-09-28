#include "base/memory/move.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "rxe/rpc/rpc_registry.h"

namespace rx::rpc {

void RpcRegistry::On(base::String name, RpcHandler handler) {
  handlers_[name] = base::move(handler);
}

bool RpcRegistry::Has(base::StringRef name) const {
  return handlers_.contains(base::String(name));
}

bool RpcRegistry::Dispatch(const RpcContext& ctx, const RpcCall& call) const {
  const RpcHandler* handler = handlers_.find(call.name);
  if (!handler) return false;
  (*handler)(ctx, call.args);
  return true;
}

void RpcRegistry::Clear() {
  handlers_.clear();
}

size_t RpcRegistry::size() const {
  return handlers_.size();
}

}  // namespace rx::rpc
