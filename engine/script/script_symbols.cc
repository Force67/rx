#include "script/script_symbols.h"

#include <cassert>

namespace rx::script {

ScriptSymbols::ScriptSymbols() = default;

StrId ScriptSymbols::Intern(ScriptStringView text) {
  const StrId id = HashStr(text);
  const u64 key = static_cast<u64>(id);
  auto it = table_.find(key);
  if (it != table_.end()) {
    // Same id must mean same bytes; a mismatch is a hash collision we refuse to
    // silently alias.
    assert(it->second.view() == text.view() && "StrId collision between symbols");
    return id;
  }
  // Store canonical, null-terminated bytes in the grow-only arena and map the id
  // to a stable view of them (storage_ is never Reset, so the view stays valid).
  ScriptStringView canonical = ArenaCopy(storage_, text);
  table_.emplace(key, canonical);
  return id;
}

ScriptStringView ScriptSymbols::Resolve(StrId id) const {
  auto it = table_.find(static_cast<u64>(id));
  return it == table_.end() ? ScriptStringView{} : it->second;
}

bool ScriptSymbols::Has(StrId id) const {
  return table_.find(static_cast<u64>(id)) != table_.end();
}

}  // namespace rx::script
