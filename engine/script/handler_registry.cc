#include "script/handler_registry.h"

#include <algorithm>
#include <cassert>

namespace rx::script {

namespace {
// Lower-bound the sorted table by key. Returns the first entry with key >= want.
template <class Vec>
auto LowerBound(Vec& table, u64 want) {
  return std::lower_bound(table.begin(), table.end(), want,
                          [](const auto& e, u64 k) { return e.key < k; });
}
}  // namespace

void HandlerRegistry::Reserve(size_t n) { table_.reserve(n); }

void HandlerRegistry::Add(ScriptStringView name, HandlerFn fn, HandlerSig sig) {
  const u64 key = static_cast<u64>(HashStr(name));
  auto it = LowerBound(table_, key);
  if (it != table_.end() && it->key == key) {
    // Same hash: either a legitimate re-registration of the same name, or a
    // (2^-64-rare) collision between two distinct names. Catch the collision at
    // registration -- startup, one-time -- so dispatch can stay hash-only on the
    // hot path without a name compare.
    assert(it->desc.name.view() == name.view() && "handler name hash collision");
    it->desc = HandlerDesc{fn, sig, name};  // replace
    return;
  }
  table_.insert(it, Entry{key, HandlerDesc{fn, sig, name}});  // keep sorted
}

bool HandlerRegistry::Has(ScriptStringView name) const {
  const u64 key = static_cast<u64>(HashStr(name));
  auto it = LowerBound(table_, key);
  return it != table_.end() && it->key == key;
}

const HandlerDesc* HandlerRegistry::Find(ScriptStringView name) const {
  const u64 key = static_cast<u64>(HashStr(name));
  auto it = LowerBound(table_, key);
  return (it != table_.end() && it->key == key) ? &it->desc : nullptr;
}

ScriptValue HandlerRegistry::Dispatch(HandlerContext& ctx, ScriptStringView name,
                                      ScriptArgs& args) const {
  const u64 key = static_cast<u64>(HashStr(name));
  auto it = LowerBound(table_, key);
  if (it == table_.end() || it->key != key || it->desc.fn == nullptr)
    return ScriptValue::Null();
  return it->desc.fn(ctx, args);
}

}  // namespace rx::script
