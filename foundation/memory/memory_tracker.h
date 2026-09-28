#ifndef RX_FOUNDATION_MEMORY_MEMORY_TRACKER_H_
#define RX_FOUNDATION_MEMORY_MEMORY_TRACKER_H_

#include <stddef.h>

#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

namespace rx {

// Category tokens label heap allocations by subsystem ("ecs", "assets", ...).
// A thread-local current category is set with MemoryCategoryScope around a
// subsystem's entry points; the operator new/delete override (new_override.cc,
// active when RX_MIMALLOC is on) stores that category with each allocation so
// a later free is charged back to the category that owns the block.
using MemoryCategory = u8;

constexpr MemoryCategory kGeneralMemoryCategory = 0;
constexpr u32 kMaxMemoryCategories = 64;
constexpr u32 kMaxMemoryCategoryNameLength = 32;

// Registers a category, or returns the existing token when `name` was already
// registered (lookup is by string content; the name is copied, truncated to
// kMaxMemoryCategoryNameLength-1). Returns kGeneralMemoryCategory when the table is full.
RX_FOUNDATION_EXPORT MemoryCategory RegisterMemoryCategory(const char* name);

RX_FOUNDATION_EXPORT MemoryCategory CurrentMemoryCategory();

// Prefer MemoryCategoryScope; exposed for the scope and for job systems that need
// to carry a category onto a worker thread manually.
RX_FOUNDATION_EXPORT void SetCurrentMemoryCategory(MemoryCategory category);

class MemoryCategoryScope {
 public:
  explicit MemoryCategoryScope(MemoryCategory category) : previous_(CurrentMemoryCategory()) {
    SetCurrentMemoryCategory(category);
  }
  ~MemoryCategoryScope() { SetCurrentMemoryCategory(previous_); }

  MemoryCategoryScope(const MemoryCategoryScope&) = delete;
  MemoryCategoryScope& operator=(const MemoryCategoryScope&) = delete;

 private:
  MemoryCategory previous_;
};

// Hot path, called by the new/delete override with the usable block size.
// Must never allocate.
RX_FOUNDATION_EXPORT void TrackAlloc(size_t bytes);
RX_FOUNDATION_EXPORT void TrackFree(size_t bytes);

namespace internal {
// Explicit-category variants used by the allocation override, which remembers
// the allocation category in its per-block footer.
RX_FOUNDATION_EXPORT void TrackAlloc(MemoryCategory category, size_t bytes);
RX_FOUNDATION_EXPORT void TrackFree(MemoryCategory category, size_t bytes);
}

// Soft budget for the category registered under `name` (registering it if
// new); 0 means no budget. Budgets only drive the debug HUD, nothing is
// enforced.
RX_FOUNDATION_EXPORT void SetMemoryCategoryBudget(const char* name, u64 bytes);

struct MemoryCategoryStats {
  const char* name = nullptr;
  i64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 budget_bytes = 0;   // 0 = no budget
  u64 alloc_count = 0;    // allocations charged since start (monotonic)
};

// Fills `out` with up to `max` registered categories, returns the count.
// Allocation-free; safe to call every frame from the debug HUD.
RX_FOUNDATION_EXPORT u32 SnapshotMemoryCategories(MemoryCategoryStats* out, u32 max);

// True when the new/delete override is compiled into this process
// (RX_MIMALLOC=ON), i.e. the counters above actually move.
RX_FOUNDATION_EXPORT bool TrackingActive();

namespace internal {
RX_FOUNDATION_EXPORT void MarkTrackingActive();
}

}  // namespace rx

#endif  // RX_FOUNDATION_MEMORY_MEMORY_TRACKER_H_
