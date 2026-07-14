#include "core/memory/memory_tracker.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace rx::mem {
namespace {

// Everything here is constinit / zero-initialized: the new/delete override
// calls TrackAlloc during static initialization of other TUs, before any
// dynamic initializer in this file could have run.
struct Slot {
  // Written once under g_register_mutex before g_count publishes the slot
  // (release/acquire), immutable afterwards; copied so ini-loaded names need
  // not outlive the call.
  char name[kMaxCategoryNameLength]{};
  std::atomic<i64> current{0};
  std::atomic<u64> peak{0};
  std::atomic<u64> budget{0};
  std::atomic<u64> allocs{0};
};

constinit Slot g_slots[kMaxCategories]{};
constinit std::atomic<u32> g_count{1};  // slot 0 is kGeneralCategory
constinit std::atomic<bool> g_active{false};
constinit std::mutex g_register_mutex;

thread_local constinit Category t_current{kGeneralCategory};

}  // namespace

Category RegisterCategory(const char* name) {
  std::lock_guard<std::mutex> lock(g_register_mutex);
  const u32 count = g_count.load(std::memory_order_relaxed);
  for (u32 i = 1; i < count; ++i) {
    if (std::strcmp(g_slots[i].name, name) == 0) return static_cast<Category>(i);
  }
  if (count >= kMaxCategories) return kGeneralCategory;
  std::strncpy(g_slots[count].name, name, kMaxCategoryNameLength - 1);
  g_count.store(count + 1, std::memory_order_release);
  return static_cast<Category>(count);
}

Category CurrentCategory() { return t_current; }

void SetCurrentCategory(Category category) { t_current = category; }

void TrackAlloc(size_t bytes) {
  Slot& slot = g_slots[t_current];
  const i64 current = slot.current.fetch_add(static_cast<i64>(bytes), std::memory_order_relaxed) +
                      static_cast<i64>(bytes);
  slot.allocs.fetch_add(1, std::memory_order_relaxed);
  u64 peak = slot.peak.load(std::memory_order_relaxed);
  while (current > 0 && static_cast<u64>(current) > peak &&
         !slot.peak.compare_exchange_weak(peak, static_cast<u64>(current),
                                          std::memory_order_relaxed)) {
  }
}

void TrackFree(size_t bytes) {
  g_slots[t_current].current.fetch_sub(static_cast<i64>(bytes), std::memory_order_relaxed);
}

void SetCategoryBudget(const char* name, u64 bytes) {
  const Category category = RegisterCategory(name);
  if (category == kGeneralCategory && std::strcmp(name, "<general>") != 0) return;  // table full
  g_slots[category].budget.store(bytes, std::memory_order_relaxed);
}

u32 SnapshotCategories(CategoryStats* out, u32 max) {
  const u32 count = g_count.load(std::memory_order_acquire);
  u32 written = 0;
  for (u32 i = 0; i < count && written < max; ++i) {
    const Slot& slot = g_slots[i];
    const char* name = i == kGeneralCategory ? "<general>" : slot.name;
    out[written++] = CategoryStats{
        .name = name,
        .current_bytes = slot.current.load(std::memory_order_relaxed),
        .peak_bytes = slot.peak.load(std::memory_order_relaxed),
        .budget_bytes = slot.budget.load(std::memory_order_relaxed),
        .alloc_count = slot.allocs.load(std::memory_order_relaxed),
    };
  }
  return written;
}

bool TrackingActive() { return g_active.load(std::memory_order_relaxed); }

namespace detail {
void MarkTrackingActive() { g_active.store(true, std::memory_order_relaxed); }
}  // namespace detail

}  // namespace rx::mem
