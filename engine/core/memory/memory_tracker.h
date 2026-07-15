#ifndef RX_CORE_MEMORY_MEMORY_TRACKER_H_
#define RX_CORE_MEMORY_MEMORY_TRACKER_H_

#include <cstddef>

#include "core/export.h"
#include "core/types.h"

namespace rx::mem {

// Category tokens label heap allocations by subsystem ("ecs", "assets", ...).
// A thread-local current category is set with CategoryScope around a
// subsystem's entry points; the operator new/delete override (new_override.cc,
// active when RX_MIMALLOC is on) stores that category with each allocation so
// a later free is charged back to the category that owns the block.
using Category = u8;

constexpr Category kGeneralCategory = 0;
constexpr u32 kMaxCategories = 64;
constexpr u32 kMaxCategoryNameLength = 32;

// Registers a category, or returns the existing token when `name` was already
// registered (lookup is by string content; the name is copied, truncated to
// kMaxCategoryNameLength-1). Returns kGeneralCategory when the table is full.
RX_CORE_EXPORT Category RegisterCategory(const char* name);

RX_CORE_EXPORT Category CurrentCategory();

// Prefer CategoryScope; exposed for the scope and for job systems that need
// to carry a category onto a worker thread manually.
RX_CORE_EXPORT void SetCurrentCategory(Category category);

class CategoryScope {
 public:
  explicit CategoryScope(Category category) : previous_(CurrentCategory()) {
    SetCurrentCategory(category);
  }
  ~CategoryScope() { SetCurrentCategory(previous_); }

  CategoryScope(const CategoryScope&) = delete;
  CategoryScope& operator=(const CategoryScope&) = delete;

 private:
  Category previous_;
};

// Hot path, called by the new/delete override with the usable block size.
// Must never allocate.
RX_CORE_EXPORT void TrackAlloc(size_t bytes);
RX_CORE_EXPORT void TrackFree(size_t bytes);

namespace detail {
// Explicit-category variants used by the allocation override, which remembers
// the allocation category in its per-block footer.
RX_CORE_EXPORT void TrackAlloc(Category category, size_t bytes);
RX_CORE_EXPORT void TrackFree(Category category, size_t bytes);
}

// Soft budget for the category registered under `name` (registering it if
// new); 0 means no budget. Budgets only drive the debug HUD, nothing is
// enforced.
RX_CORE_EXPORT void SetCategoryBudget(const char* name, u64 bytes);

struct CategoryStats {
  const char* name = nullptr;
  i64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 budget_bytes = 0;   // 0 = no budget
  u64 alloc_count = 0;    // allocations charged since start (monotonic)
};

// Fills `out` with up to `max` registered categories, returns the count.
// Allocation-free; safe to call every frame from the debug HUD.
RX_CORE_EXPORT u32 SnapshotCategories(CategoryStats* out, u32 max);

// True when the new/delete override is compiled into this process
// (RX_MIMALLOC=ON), i.e. the counters above actually move.
RX_CORE_EXPORT bool TrackingActive();

namespace detail {
RX_CORE_EXPORT void MarkTrackingActive();
}

}  // namespace rx::mem

#endif  // RX_CORE_MEMORY_MEMORY_TRACKER_H_
