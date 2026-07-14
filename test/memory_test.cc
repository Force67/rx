// Acceptance test for engine/core/memory: category tracker, frame arena,
// chunk pool, small vector and the memory config. Pure CPU, no GPU needed.
// The tracker assertions only run when the new/delete override is compiled in
// (RX_MIMALLOC=ON); otherwise they are skipped so the test still passes.
#include <cstdio>
#include <cstring>

#include "core/memory/chunk_pool.h"
#include "core/memory/frame_arena.h"
#include "core/memory/memory_config.h"
#include "core/memory/memory_tracker.h"
#include "core/memory/small_vector.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                    \
    }                                                                  \
  } while (0)

rx::mem::CategoryStats FindCategory(const char* name) {
  rx::mem::CategoryStats stats[rx::mem::kMaxCategories];
  const rx::u32 count = rx::mem::SnapshotCategories(stats, rx::mem::kMaxCategories);
  for (rx::u32 i = 0; i < count; ++i) {
    if (std::strcmp(stats[i].name, name) == 0) return stats[i];
  }
  return {};
}

void TestTracker() {
  const rx::mem::Category category = rx::mem::RegisterCategory("test-cat");
  CHECK(category != rx::mem::kGeneralCategory);
  CHECK(rx::mem::RegisterCategory("test-cat") == category);  // idempotent by name

  if (!rx::mem::TrackingActive()) {
    std::puts("tracker: RX_MIMALLOC off, skipping counter checks");
    return;
  }

  const rx::mem::CategoryStats before = FindCategory("test-cat");
  void* block = nullptr;
  {
    rx::mem::CategoryScope scope(category);
    block = ::operator new(1 << 16);
  }
  const rx::mem::CategoryStats held = FindCategory("test-cat");
  CHECK(held.current_bytes - before.current_bytes >= 1 << 16);
  CHECK(held.peak_bytes >= static_cast<rx::u64>(held.current_bytes));
  CHECK(held.alloc_count > before.alloc_count);

  {
    rx::mem::CategoryScope scope(category);
    ::operator delete(block);
  }
  const rx::mem::CategoryStats released = FindCategory("test-cat");
  CHECK(released.current_bytes == before.current_bytes);  // exact: mi_usable_size both sides

  rx::mem::SetCategoryBudget("test-cat", 123);
  CHECK(FindCategory("test-cat").budget_bytes == 123);
}

void TestFrameArena() {
  rx::mem::FrameArena arena;
  arena.Init(4096);

  void* a = arena.Alloc(100, 8);
  void* b = arena.Alloc(100, 64);
  CHECK(a != nullptr && b != nullptr && a != b);
  CHECK(reinterpret_cast<uintptr_t>(b) % 64 == 0);
  CHECK(arena.stats().offset_bytes >= 200);
  CHECK(arena.stats().overflow_allocs == 0);

  // Larger than capacity: falls back to the heap, flagged in stats.
  void* big = arena.Alloc(8192, 16);
  CHECK(big != nullptr);
  CHECK(arena.stats().overflow_allocs == 1);
  CHECK(arena.stats().overflow_bytes == 8192);

  const size_t high_water = arena.stats().high_water_bytes;
  arena.Reset();
  CHECK(arena.stats().offset_bytes == 0);
  CHECK(arena.stats().overflow_bytes == 0);
  CHECK(arena.stats().high_water_bytes == high_water);  // survives reset

  float* floats = arena.AllocArray<float>(16);
  CHECK(floats != nullptr);
  arena.Shutdown();
}

void TestChunkPool() {
  rx::mem::ChunkPool pool;
  pool.Reserve(3);
  CHECK(pool.stats().total_chunks >= 3);
  CHECK(pool.stats().free_chunks == pool.stats().total_chunks);

  void* a = pool.Acquire();
  void* b = pool.Acquire();
  CHECK(a != nullptr && b != nullptr && a != b);
  CHECK(reinterpret_cast<uintptr_t>(a) % rx::mem::ChunkPool::kChunkAlign == 0);
  // Chunks are writable across their whole extent.
  std::memset(a, 0xab, rx::mem::ChunkPool::kChunkSize);

  const size_t total = pool.stats().total_chunks;
  CHECK(pool.stats().free_chunks == total - 2);
  pool.Release(a);
  pool.Release(b);
  CHECK(pool.stats().free_chunks == total);
  CHECK(pool.Acquire() == b);  // LIFO reuse keeps hot chunks hot
}

struct Probe {
  static inline int live = 0;
  int value = 0;
  explicit Probe(int v) : value(v) { ++live; }
  Probe(Probe&& other) noexcept : value(other.value) { ++live; }
  ~Probe() { --live; }
};

void TestSmallVector() {
  {
    rx::mem::SmallVector<Probe, 4> vec;
    for (int i = 0; i < 3; ++i) vec.emplace_back(i);
    const void* inline_data = vec.data();
    for (int i = 3; i < 32; ++i) vec.emplace_back(i);  // grows to the heap
    CHECK(vec.size() == 32);
    CHECK(vec.data() != inline_data);
    for (int i = 0; i < 32; ++i) CHECK(vec[static_cast<size_t>(i)].value == i);

    rx::mem::SmallVector<Probe, 4> moved(std::move(vec));
    CHECK(moved.size() == 32);
    CHECK(vec.size() == 0);
    CHECK(Probe::live == 32);
  }
  CHECK(Probe::live == 0);

  rx::mem::SmallVector<int, 8> ints;
  for (int i = 0; i < 8; ++i) ints.push_back(i);
  CHECK(ints.capacity() == 8);  // still inline at exactly N
  ints.clear();
  CHECK(ints.empty());
}

void TestConfig() {
  rx::mem::MemoryConfig config = rx::mem::DefaultMemoryConfig("steamdeck");
  CHECK(config.preset == "steamdeck");
  CHECK(config.frame_arena_bytes == 6u << 20);

  rx::mem::ParseMemoryConfigText(
      "; comment\n"
      "[arena]\n"
      "frame_mb = 2\n"
      "[pools]\n"
      "ecs_chunks = 32\n"
      "[budgets]\n"
      "ecs = 7  # MiB\n"
      "custom-cat = 9\n"
      "bogus line without equals\n",
      config);
  CHECK(config.frame_arena_bytes == 2u << 20);
  CHECK(config.ecs_chunk_reserve == 32);
  bool saw_ecs = false, saw_custom = false;
  for (const auto& budget : config.budgets) {
    if (budget.name == "ecs") saw_ecs = budget.bytes == 7u << 20;
    if (budget.name == "custom-cat") saw_custom = budget.bytes == 9u << 20;
  }
  CHECK(saw_ecs && saw_custom);

  CHECK(rx::mem::DefaultMemoryConfig("nonsense").preset == "desktop");
}

}  // namespace

int main() {
  TestTracker();
  TestFrameArena();
  TestChunkPool();
  TestSmallVector();
  TestConfig();
  if (g_failures) {
    std::fprintf(stderr, "memory_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("memory_test: ok");
  return 0;
}
