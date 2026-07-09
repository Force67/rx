#include "rpc/rpc_registry.h"

namespace rx::rpc {

void RpcRegistry::On(std::string name, RpcHandler handler) {
  handlers_[std::move(name)] = std::move(handler);
}

bool RpcRegistry::Has(std::string_view name) const {
  return handlers_.find(std::string(name)) != handlers_.end();
}

bool RpcRegistry::Dispatch(const RpcContext& ctx, const RpcCall& call) const {
  auto it = handlers_.find(call.name);
  if (it == handlers_.end()) return false;
  it->second(ctx, call.args);
  return true;
}

void RpcRegistry::Clear() {
  handlers_.clear();
}

size_t RpcRegistry::size() const {
  return handlers_.size();
}

}  // namespace rx::rpc
