#ifndef RX_SCRIPT_HANDLER_REGISTRY_H_
#define RX_SCRIPT_HANDLER_REGISTRY_H_

#include <cassert>
#include <initializer_list>
#include <vector>

#include "core/export.h"
#include "core/types.h"
#include "script/script_string.h"
#include "script/script_value.h"

namespace rx::script {

struct HandlerContext;  // the per-call service locator (handler_context.h)

// A bound script handler: the file-private unpacking trampoline a category emits.
// A plain function pointer, not std::function -- no capture, no allocation, and a
// stable calling convention every runtime and the wire path can share.
using HandlerFn = ScriptValue (*)(HandlerContext&, ScriptArgs&);

// Upper bound on a handler's parameter count. Script functions never approach
// this; keeping the params inline (no std::vector) means registration allocates
// nothing per command.
inline constexpr u32 kMaxHandlerParams = 12;

// The typed signature recorded alongside a handler -- the machine-readable record
// of the free function's signature, the one source of truth for arg validation,
// wire codecs and generating outward runtime stubs. Trivially copyable: params
// live inline, not in a heap vector, so building one is allocation-free. The
// braced call site `{kVoid, {kEntity, kVec3}}` still works: the initializer_list
// is backed by a stack array, copied into `params` here -- no heap.
struct HandlerSig {
  ScriptType ret = ScriptType::kVoid;
  u8 count = 0;
  ScriptType params[kMaxHandlerParams]{};

  constexpr HandlerSig() = default;
  constexpr HandlerSig(ScriptType r, std::initializer_list<ScriptType> p) : ret(r) {
    for (ScriptType t : p) {
      assert(count < kMaxHandlerParams && "handler exceeds kMaxHandlerParams");
      if (count < kMaxHandlerParams) params[count++] = t;  // never overflow the buffer
    }
  }
};

struct HandlerDesc {
  HandlerFn fn = nullptr;
  HandlerSig sig;
  // Points at the caller's name bytes. Command names are string literals (static
  // storage), so this is a view, never a copied std::string -- zero allocation.
  ScriptStringView name;
};

// Name -> handler. Categories fill it once at engine start via their
// SetupCategoryCommands() free functions; runtimes dispatch decoded calls into
// it. Backed by a flat table sorted on the interned name hash: one growing buffer
// instead of a node allocation per command, and a cache-friendly binary search
// on the hot path rather than a pointer-chasing bucket walk.
class HandlerRegistry {
 public:
  // Optional: reserve capacity before bulk registration to avoid regrowth.
  RX_SCRIPT_EXPORT void Reserve(size_t n);

  // `name` must have static lifetime (a string literal); the descriptor stores a
  // view of it, not a copy.
  RX_SCRIPT_EXPORT void Add(ScriptStringView name, HandlerFn fn, HandlerSig sig);
  RX_SCRIPT_EXPORT bool Has(ScriptStringView name) const;

  // Valid until the next Add() (a flat table relocates on insert). Registration
  // completes at startup before any lookup, so this is not a concern in practice.
  RX_SCRIPT_EXPORT const HandlerDesc* Find(ScriptStringView name) const;

  // Looks up the name and invokes its handler. An unregistered name returns Null
  // and does nothing -- a runtime may emit names this build does not implement,
  // and that is dropped, not an error (mirrors rpc::RpcRegistry). A returned
  // string value views the context's scratch arena; the caller must consume it
  // before that arena is reset (see HandlerContext::scratch).
  RX_SCRIPT_EXPORT ScriptValue Dispatch(HandlerContext& ctx, ScriptStringView name,
                                        ScriptArgs& args) const;

  size_t size() const { return table_.size(); }

 private:
  struct Entry {
    u64 key;  // HashStr(name)
    HandlerDesc desc;
  };
  std::vector<Entry> table_;  // sorted by key
};

}  // namespace rx::script

#endif  // RX_SCRIPT_HANDLER_REGISTRY_H_
