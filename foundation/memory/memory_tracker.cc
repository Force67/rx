#include "base/atomic.h"
#include "base/threading/lock_guard.h"
#include "base/threading/mutex.h"
#include "foundation/memory/memory_tracker.h"

#include <string.h>

namespace rx {
namespace {

// Everything here is constinit / zero-initialized: the new/delete override
// calls TrackAlloc during static initialization of other TUs, before any
// dynamic initializer in this file could have run.
struct Slot {
  // Written once under g_register_mutex before g_count publishes the slot
  // (release/acquire), immutable afterwards; copied so ini-loaded names need
  // not outlive the call.
  char name[kMaxMemoryCategoryNameLength]{};
  base::Atomic<i64> current{0};
  base::Atomic<u64> peak{0};
  base::Atomic<u64> budget{0};
  base::Atomic<u64> allocs{0};
};

constinit Slot g_slots[kMaxMemoryCategories]{};
constinit base::Atomic<u32> g_count{1};  // slot 0 is kGeneralMemoryCategory
constinit base::Atomic<bool> g_active{false};
constinit base::Mutex g_register_mutex;

thread_local constinit MemoryCategory t_current{kGeneralMemoryCategory};

}  // namespace

MemoryCategory RegisterMemoryCategory(const char* name) {
  base::LockGuard<base::Mutex> lock(g_register_mutex);
  const u32 count = g_count.load(base::memory_order_relaxed);
  for (u32 i = 1; i < count; ++i) {
    if (::strcmp(g_slots[i].name, name) == 0) return static_cast<MemoryCategory>(i);
  }
  if (count >= kMaxMemoryCategories) return kGeneralMemoryCategory;
  ::strncpy(g_slots[count].name, name, kMaxMemoryCategoryNameLength - 1);
  g_count.store(count + 1, base::memory_order_release);
  return static_cast<MemoryCategory>(count);
}

MemoryCategory CurrentMemoryCategory() { return t_current; }

void SetCurrentMemoryCategory(MemoryCategory category) { t_current = category; }

void TrackAlloc(size_t bytes) {
  internal::TrackAlloc(t_current, bytes);
}

void TrackFree(size_t bytes) { internal::TrackFree(t_current, bytes); }

namespace internal {

void TrackAlloc(MemoryCategory category, size_t bytes) {
  Slot& slot = g_slots[category];
  const i64 current = slot.current.fetch_add(static_cast<i64>(bytes), base::memory_order_relaxed) +
                      static_cast<i64>(bytes);
  slot.allocs.fetch_add(1, base::memory_order_relaxed);
  u64 peak = slot.peak.load(base::memory_order_relaxed);
  while (current > 0 && static_cast<u64>(current) > peak &&
         !slot.peak.compare_exchange_weak(peak, static_cast<u64>(current),
                                          base::memory_order_relaxed)) {
  }
}

void TrackFree(MemoryCategory category, size_t bytes) {
  g_slots[category].current.fetch_sub(static_cast<i64>(bytes), base::memory_order_relaxed);
}

}  // namespace internal

void SetMemoryCategoryBudget(const char* name, u64 bytes) {
  const MemoryCategory category = RegisterMemoryCategory(name);
  if (category == kGeneralMemoryCategory && ::strcmp(name, "<general>") != 0) return;  // table full
  g_slots[category].budget.store(bytes, base::memory_order_relaxed);
}

u32 SnapshotMemoryCategories(MemoryCategoryStats* out, u32 max) {
  const u32 count = g_count.load(base::memory_order_acquire);
  u32 written = 0;
  for (u32 i = 0; i < count && written < max; ++i) {
    const Slot& slot = g_slots[i];
    const char* name = i == kGeneralMemoryCategory ? "<general>" : slot.name;
    out[written++] = MemoryCategoryStats{
        .name = name,
        .current_bytes = slot.current.load(base::memory_order_relaxed),
        .peak_bytes = slot.peak.load(base::memory_order_relaxed),
        .budget_bytes = slot.budget.load(base::memory_order_relaxed),
        .alloc_count = slot.allocs.load(base::memory_order_relaxed),
    };
  }
  return written;
}

bool TrackingActive() { return g_active.load(base::memory_order_relaxed); }

namespace internal {
void MarkTrackingActive() { g_active.store(true, base::memory_order_relaxed); }
}  // namespace internal

}  // namespace rx
