#ifndef RX_SCRIPT_HANDLER_CONTEXT_H_
#define RX_SCRIPT_HANDLER_CONTEXT_H_

#include <cassert>

#include "script/script_arena.h"
#include "script/script_string.h"
#include "script/script_symbols.h"

namespace rx::ecs {
class World;
}

namespace rx::script {

// The per-call context: concrete, non-owning pointers to the engine, wired once
// at engine start. There are NO virtual interfaces here -- a handler reaches the
// engine by calling it directly through these pointers. ecs::World is the shared
// component backbone most handlers operate on (position, name, etc. are just
// components); the script primitives (symbol interner, per-call scratch heap, a
// log sink) ride along.
//
// A system that is a separate service (an audio device, a physics world) adds
// its own concrete pointer here when it gains handlers. If a system does not
// want this header to name it, it supplies an app-defined context extension --
// still a direct pointer, never a vtable. The forward declaration of ecs::World
// keeps this substrate header from pulling in the ECS implementation.
struct HandlerContext {
  ecs::World* world = nullptr;
  ScriptSymbols* symbols = nullptr;
  // Per-call scratch heap: content strings a handler returns are ArenaCopy'd here
  // (see ScriptArena). The runtime must consume/marshal a call's return value
  // before it Reset()s this arena -- in practice reset once per frame, not
  // mid-call -- or a returned ScriptStringView dangles.
  ScriptArena* scratch = nullptr;

  // A portable log sink (function pointer + user data, no std::function) so the
  // context stays a plain struct any runtime can populate.
  void (*log_sink)(void* user, ScriptStringView msg) = nullptr;
  void* log_user = nullptr;

  ecs::World& Ecs() const {
    assert(world && "ecs::World not wired into HandlerContext");
    return *world;
  }
  ScriptSymbols& Syms() const {
    assert(symbols && "ScriptSymbols not wired into HandlerContext");
    return *symbols;
  }
  ScriptArena& Heap() const {
    assert(scratch && "scratch ScriptArena not wired into HandlerContext");
    return *scratch;
  }
  void Log(ScriptStringView msg) const {
    if (log_sink) log_sink(log_user, msg);
  }
};

}  // namespace rx::script

#endif  // RX_SCRIPT_HANDLER_CONTEXT_H_
