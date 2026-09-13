#ifndef RX_SCRIPT_SCRIPT_SYMBOLS_H_
#define RX_SCRIPT_SCRIPT_SYMBOLS_H_

#include <unordered_map>

#include "core/export.h"
#include "core/types.h"
#include "script/script_arena.h"
#include "script/script_string.h"

namespace rx::script {

// The interner: the single owner of every script SYMBOL (handler/category
// names, keywords, animation-event names, editor ids). Symbols are a bounded,
// repeated set compared by identity, so interning gives one controlled place
// (dump, count, budget), O(1) comparison via StrId, and storage stable for the
// interner's lifetime: a symbol crosses the handler boundary as a bare
// ScriptStringView with no ownership question.
//
// CONTENT strings (messages, user text) are deliberately NOT interned: they are
// unbounded and transient and belong in the per-call ScriptArena. Keeping the
// populations apart is what stops the interner growing without bound.
class ScriptSymbols {
 public:
  RX_SCRIPT_EXPORT ScriptSymbols();

  // Intern `text`, returning its StrId. Idempotent: the same text always returns
  // the same id and stores the bytes only once. Detects (asserts) the vanishingly
  // rare case of two different texts hashing to one StrId.
  RX_SCRIPT_EXPORT StrId Intern(ScriptStringView text);

  // The canonical bytes for an id, stable for this interner's lifetime, or empty
  // if the id was never interned.
  RX_SCRIPT_EXPORT ScriptStringView Resolve(StrId id) const;
  RX_SCRIPT_EXPORT bool Has(StrId id) const;

  size_t size() const { return table_.size(); }

 private:
  ScriptArena storage_;  // grow-only canonical bytes (never Reset)
  std::unordered_map<u64, ScriptStringView> table_;  // StrId -> canonical view
};

}  // namespace rx::script

#endif  // RX_SCRIPT_SCRIPT_SYMBOLS_H_
