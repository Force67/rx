#ifndef RX_SCRIPT_SCRIPT_ARENA_H_
#define RX_SCRIPT_SCRIPT_ARENA_H_

#include <cstddef>

#include "core/export.h"
#include "core/types.h"
#include "script/script_string.h"

namespace rx::script {

// A linear (bump) allocator for transient script memory: the per-call argument
// stacks and the content strings handlers produce. Allocate freely during a call
// or frame, then Reset() to reclaim it all at once -- no per-argument malloc/free
// churn, and script memory stays bounded and separately accountable from the
// global heap. Grows in chained blocks; Reset keeps the blocks for reuse so the
// steady state does not allocate at all.
//
// This is the "script heap" seam. Today it owns its blocks via operator new;
// when rx's category allocator lands, back NewBlock() with a dedicated script
// memory category and bytes_used()/high_water() feed straight into the HUD.
class ScriptArena {
 public:
  RX_SCRIPT_EXPORT explicit ScriptArena(size_t block_size = 64 * 1024);
  RX_SCRIPT_EXPORT ~ScriptArena();

  ScriptArena(const ScriptArena&) = delete;
  ScriptArena& operator=(const ScriptArena&) = delete;

  // Bump-allocate `bytes` with `align`. Never returns null (throws std::bad_alloc
  // only if the backing new fails). Lifetime: until the next Reset().
  RX_SCRIPT_EXPORT void* Alloc(size_t bytes, size_t align = alignof(std::max_align_t));

  // Reclaim everything at once. Blocks are retained for reuse, not freed.
  RX_SCRIPT_EXPORT void Reset();

  size_t bytes_used() const { return used_; }
  size_t high_water() const { return high_water_; }

 private:
  struct Block {
    u8* base = nullptr;
    size_t cap = 0;
    size_t used = 0;
    Block* next = nullptr;
  };
  Block* NewBlock(size_t min_bytes);

  Block* head_ = nullptr;        // current block being filled
  Block* free_list_ = nullptr;   // blocks freed by Reset(), kept for reuse
  size_t block_size_ = 0;
  size_t used_ = 0;              // live bytes since last Reset
  size_t high_water_ = 0;        // peak used_, for budgeting
};

// Copy `s` into `arena`, null-terminated, and return a view of it. The view is
// valid until the arena's next Reset(); the runtime must consume/marshal a
// handler's return value before it resets the scratch arena (see HandlerContext
// and HandlerRegistry::Dispatch). This is how a handler puts a content string on
// the value stack without touching the global heap -- the arena owns the bytes,
// the ScriptValue only views them.
RX_SCRIPT_EXPORT ScriptStringView ArenaCopy(ScriptArena& arena, ScriptStringView s);

}  // namespace rx::script

#endif  // RX_SCRIPT_SCRIPT_ARENA_H_
