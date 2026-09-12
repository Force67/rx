// Acceptance test for the chunked ECS storage: spawn storms (the old
// exact-size column resize was O(n^2) and relocated rows bitwise), non-POD
// components with interior pointers (bitwise relocation would corrupt them),
// archetype transitions, swap-remove churn and chunk reclamation.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/memory/chunk_pool.h"
#include "ecs/world.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

struct Position {
  float x = 0, y = 0, z = 0;
};

struct Velocity {
  float x = 0, y = 0, z = 0;
};

struct Tag {
  rx::u8 marker = 0;
};

// Interior self-pointer: only valid if every relocation went through the move
// constructor. The old byte-vector column grow memcpy'd rows and would leave
// self pointing at the stale block.
struct SelfRef {
  SelfRef* self;
  std::string payload;
  int id;

  explicit SelfRef(int i) : self(this), payload("payload-" + std::to_string(i)), id(i) {}
  SelfRef(SelfRef&& other) noexcept
      : self(this), payload(std::move(other.payload)), id(other.id) {}
  bool valid() const { return self == this; }
};

struct alignas(32) WideComponent {
  double values[4] = {1, 2, 3, 4};
};

void TestWorldStatsTrackStoragePressure() {
  rx::ecs::World world;
  rx::ecs::Entity first = world.Create();
  rx::ecs::Entity second = world.Create();
  world.Add(first, Position{});
  world.Add(second, Position{});

  rx::ecs::World::Stats stats = world.stats();
  CHECK(stats.entity_count == 2);
  CHECK(stats.entity_slots == 2);
  CHECK(stats.archetype_count == 2);
  CHECK(stats.live_component_bytes == 2 * sizeof(Position));
  CHECK(stats.component_capacity_bytes >= stats.live_component_bytes);

  world.Destroy(first);
  stats = world.stats();
  CHECK(stats.entity_count == 1);
  CHECK(stats.entity_slots == 2);
  CHECK(stats.live_component_bytes == sizeof(Position));

  world.Create();
  stats = world.stats();
  CHECK(stats.entity_count == 2);
  CHECK(stats.entity_slots == 2);
}

void TestSpawnStormAndIteration() {
  rx::ecs::World world;
  constexpr int kCount = 20000;  // thousands of rows -> many chunks per archetype

  std::vector<rx::ecs::Entity> entities;
  entities.reserve(kCount);
  for (int i = 0; i < kCount; ++i) {
    rx::ecs::Entity entity = world.Create();
    world.Add(entity, Position{float(i), float(i) * 2, 0});
    world.Add(entity, Velocity{1, 0, 0});
    entities.push_back(entity);
  }
  CHECK(world.entity_count() == kCount);

  int seen = 0;
  bool values_ok = true;
  world.Each<Position, Velocity>([&](rx::ecs::Entity, Position& pos, Velocity& vel) {
    values_ok = values_ok && pos.y == pos.x * 2 && vel.x == 1;
    pos.x += vel.x;
    ++seen;
  });
  CHECK(seen == kCount);
  CHECK(values_ok);

  // Random access agrees with iteration results.
  CHECK(world.Get<Position>(entities[12345])->x == 12346.0f);

  for (rx::ecs::Entity entity : entities) world.Destroy(entity);
  CHECK(world.entity_count() == 0);
}

void TestNonPodRelocation() {
  rx::ecs::World world;
  constexpr int kCount = 5000;

  std::vector<rx::ecs::Entity> entities;
  for (int i = 0; i < kCount; ++i) {
    rx::ecs::Entity entity = world.Create();
    world.Add(entity, SelfRef(i));
    entities.push_back(entity);
  }

  int valid = 0;
  world.Each<SelfRef>([&](rx::ecs::Entity, SelfRef& ref) {
    if (ref.valid() && ref.payload == "payload-" + std::to_string(ref.id)) ++valid;
  });
  CHECK(valid == kCount);

  // Archetype transition moves every component through move_construct.
  for (int i = 0; i < kCount; i += 2) world.Add(entities[static_cast<size_t>(i)], Tag{1});
  valid = 0;
  world.Each<SelfRef, Tag>([&](rx::ecs::Entity, SelfRef& ref, Tag&) {
    if (ref.valid()) ++valid;
  });
  CHECK(valid == kCount / 2);

  // Swap-remove churn: destroy every third entity, the swapped-in survivors
  // must stay valid.
  for (int i = 0; i < kCount; i += 3) world.Destroy(entities[static_cast<size_t>(i)]);
  valid = 0;
  int total = 0;
  world.Each<SelfRef>([&](rx::ecs::Entity, SelfRef& ref) {
    ++total;
    if (ref.valid()) ++valid;
  });
  CHECK(valid == total);
  CHECK(total == kCount - (kCount + 2) / 3);
}

void TestComponentAlignment() {
  rx::ecs::World world;
  for (int i = 0; i < 100; ++i) {
    rx::ecs::Entity entity = world.Create();
    world.Add(entity, WideComponent{});
    CHECK(reinterpret_cast<uintptr_t>(world.Get<WideComponent>(entity)) % 32 == 0);
  }
}

void TestChunkReclamation() {
  const size_t free_before = rx::mem::GlobalChunkPool().stats().free_chunks;
  {
    rx::ecs::World world;
    std::vector<rx::ecs::Entity> entities;
    for (int i = 0; i < 10000; ++i) {
      rx::ecs::Entity entity = world.Create();
      world.Add(entity, Position{});
      entities.push_back(entity);
    }
    // The world holds chunks now...
    CHECK(rx::mem::GlobalChunkPool().stats().free_chunks <
          rx::mem::GlobalChunkPool().stats().total_chunks);
    for (rx::ecs::Entity entity : entities) world.Destroy(entity);
  }
  // ...and returns every one of them after destruction.
  CHECK(rx::mem::GlobalChunkPool().stats().free_chunks >= free_before);
  CHECK(rx::mem::GlobalChunkPool().stats().free_chunks ==
        rx::mem::GlobalChunkPool().stats().total_chunks);
}

void TestAddRemoveOverwrite() {
  rx::ecs::World world;
  rx::ecs::Entity entity = world.Create();
  world.Add(entity, Position{1, 2, 3});
  world.Add(entity, Position{4, 5, 6});  // overwrite destructs + reconstructs in place
  CHECK(world.Get<Position>(entity)->x == 4);
  world.Remove<Position>(entity);
  CHECK(world.Get<Position>(entity) == nullptr);
  CHECK(!world.Has<Position>(entity));
  world.Add(entity, Position{7, 8, 9});
  CHECK(world.Get<Position>(entity)->x == 7);
}

// CreateBatch is the ingress cooked world data uses: one append into one
// archetype, columns written as runs. The runs must tile the batch exactly,
// stop at every chunk boundary, and leave the entities indistinguishable from
// ones built with Create + Add.
void TestCreateBatchFillsColumnRuns() {
  rx::ecs::World world;
  const rx::ecs::Signature signature = rx::ecs::MakeSignature(
      {rx::ecs::GetComponentId<Position>(), rx::ecs::GetComponentId<Velocity>()});

  // Enough rows to cross several 16 KiB chunks for a 24-byte row.
  constexpr rx::u32 kCount = 5000;
  std::vector<rx::ecs::Entity> created;
  world.CreateBatch(signature, kCount, [&](const rx::ecs::EntityBatch& batch) {
    CHECK(batch.count() == kCount);
    rx::u32 covered = 0;
    while (covered < batch.count()) {
      rx::u32 run = 0;
      auto* positions = static_cast<Position*>(
          batch.Column(rx::ecs::GetComponentId<Position>(), covered, &run));
      rx::u32 velocity_run = 0;
      auto* velocities = static_cast<Velocity*>(
          batch.Column(rx::ecs::GetComponentId<Velocity>(), covered, &velocity_run));
      CHECK(positions != nullptr);
      CHECK(velocities != nullptr);
      CHECK(run > 0);
      // Both columns live in the same chunk, so both runs end together.
      CHECK(run == velocity_run);
      // A run stops at the chunk boundary. Getting this wrong is not a missed
      // optimization: the caller memcpys `run` rows from one pointer, so a run
      // that overstates the chunk writes into the next one.
      const rx::u32 rows_per_chunk = 16 * 1024 / (sizeof(Position) + sizeof(Velocity));
      CHECK(run == std::min(rows_per_chunk - covered % rows_per_chunk, batch.count() - covered));
      for (rx::u32 i = 0; i < run; ++i) {
        new (positions + i) Position{static_cast<float>(covered + i), 0, 0};
        new (velocities + i) Velocity{0, static_cast<float>(covered + i), 0};
      }
      covered += run;
    }
    CHECK(covered == batch.count());
    for (rx::u32 i = 0; i < batch.count(); ++i) created.push_back(batch.EntityAt(i));
  });

  CHECK(world.entity_count() == kCount);
  CHECK(created.size() == kCount);
  CHECK(world.stats().archetype_count == 2);  // the empty root plus the batch's

  int visited = 0;
  world.Each<Position, Velocity>(
      [&](rx::ecs::Entity, Position& position, Velocity& velocity) {
        CHECK(position.x == velocity.y);
        ++visited;
      });
  CHECK(visited == static_cast<int>(kCount));

  // Batch entities are ordinary entities: alive, addressable, destroyable.
  for (rx::u32 i = 0; i < kCount; ++i) {
    CHECK(world.IsAlive(created[i]));
    const Position* position = world.Get<Position>(created[i]);
    CHECK(position != nullptr && position->x == static_cast<float>(i));
  }
  CHECK(rx::ecs::GetComponentInfo(rx::ecs::GetComponentId<Position>()).trivially_copyable);
  CHECK(!rx::ecs::GetComponentInfo(rx::ecs::GetComponentId<SelfRef>()).trivially_copyable);

  world.Destroy(created[0]);
  CHECK(!world.IsAlive(created[0]));
  CHECK(world.entity_count() == kCount - 1);

  // A second batch into the same archetype starts partway through it, so the
  // column pointers have to be offset by the rows already there. Filling only
  // the second batch and reading back both is what catches an implementation
  // that ignores where the batch begins.
  std::vector<rx::ecs::Entity> second;
  world.CreateBatch(signature, 700, [&](const rx::ecs::EntityBatch& batch) {
    rx::u32 covered = 0;
    while (covered < batch.count()) {
      rx::u32 run = 0;
      auto* positions =
          static_cast<Position*>(batch.Column(rx::ecs::GetComponentId<Position>(), covered, &run));
      rx::u32 velocity_run = 0;
      auto* velocities =
          static_cast<Velocity*>(batch.Column(rx::ecs::GetComponentId<Velocity>(), covered,
                                              &velocity_run));
      CHECK(run > 0 && run == velocity_run);
      for (rx::u32 i = 0; i < run; ++i) {
        new (positions + i) Position{-1.0f, 0, 0};
        new (velocities + i) Velocity{};
      }
      covered += run;
    }
    for (rx::u32 i = 0; i < batch.count(); ++i) second.push_back(batch.EntityAt(i));
  });
  // The first batch's rows are untouched: a batch that wrote from row zero
  // would have overwritten them with -1.
  int untouched = 0;
  for (rx::u32 i = 1; i < kCount; ++i) {
    const Position* position = world.Get<Position>(created[i]);
    if (position && position->x == static_cast<float>(i)) ++untouched;
  }
  CHECK(untouched == static_cast<int>(kCount) - 1);
  for (rx::ecs::Entity entity : second) {
    const Position* position = world.Get<Position>(entity);
    CHECK(position != nullptr && position->x == -1.0f);
  }
}

// A batch must reuse the free list Destroy fills, and a zero-row batch must not
// invoke the callback at all (there is nothing to initialize, and a caller that
// wrote to a zero-length run would be writing into another entity's row).
void TestCreateBatchReusesSlotsAndSkipsEmpty() {
  rx::ecs::World world;
  rx::ecs::Entity first = world.Create();
  world.Add(first, Position{1, 2, 3});
  const rx::u32 slots_before = world.stats().entity_slots;
  world.Destroy(first);

  const rx::ecs::Signature signature =
      rx::ecs::MakeSignature({rx::ecs::GetComponentId<Position>()});
  rx::ecs::Entity reused{};
  world.CreateBatch(signature, 1, [&](const rx::ecs::EntityBatch& batch) {
    rx::u32 run = 0;
    auto* positions =
        static_cast<Position*>(batch.Column(rx::ecs::GetComponentId<Position>(), 0, &run));
    CHECK(run == 1);
    new (positions) Position{4, 5, 6};
    reused = batch.EntityAt(0);
  });
  CHECK(world.stats().entity_slots == slots_before);  // the freed slot came back
  CHECK(reused.index == first.index);
  CHECK(reused.generation != first.generation);  // ... with a fresh generation
  CHECK(!world.IsAlive(first));
  CHECK(world.IsAlive(reused));

  bool called = false;
  world.CreateBatch(signature, 0, [&](const rx::ecs::EntityBatch&) { called = true; });
  CHECK(!called);
  CHECK(world.entity_count() == 1);

  // An unknown component yields no run rather than a pointer into the wrong
  // column, so a loader that asks for something the archetype lacks fails its
  // own check instead of scribbling.
  world.CreateBatch(signature, 1, [&](const rx::ecs::EntityBatch& batch) {
    rx::u32 run = 0;
    new (batch.Column(rx::ecs::GetComponentId<Position>(), 0, &run)) Position{};
    rx::u32 missing_run = 7;
    CHECK(batch.Column(rx::ecs::GetComponentId<Velocity>(), 0, &missing_run) == nullptr);
    CHECK(missing_run == 0);
    CHECK(batch.Column(rx::ecs::GetComponentId<Position>(), 1, &missing_run) == nullptr);
  });
}

void TestStructuralMutationDuringIteration() {
  rx::ecs::World world;
  rx::ecs::Entity first = world.Create();
  rx::ecs::Entity second = world.Create();
  world.Add(first, SelfRef(1));
  world.Add(second, SelfRef(2));

  int calls = 0;
  world.Each<SelfRef>([&](rx::ecs::Entity entity, SelfRef&) {
    ++calls;
    if (calls == 1) world.Destroy(entity);
  });
  // Swap-removal shrinks the archetype under the iterator. The moved-in row is
  // skipped this pass, but destroyed tail storage must never be visited.
  CHECK(calls == 1);
  CHECK(world.entity_count() == 1);

  int survivors = 0;
  world.Each<SelfRef>([&](rx::ecs::Entity, SelfRef& ref) {
    CHECK(ref.valid());
    ++survivors;
  });
  CHECK(survivors == 1);

  calls = 0;
  bool added = false;
  world.Each<SelfRef>([&](rx::ecs::Entity, SelfRef&) {
    ++calls;
    if (!added) {
      added = true;
      rx::ecs::Entity appended = world.Create();
      world.Add(appended, SelfRef(3));
    }
  });
  CHECK(calls == 1);  // a pure tail append waits until the next pass
  CHECK(world.entity_count() == 2);
}

}  // namespace

int main() {
  TestWorldStatsTrackStoragePressure();
  TestSpawnStormAndIteration();
  TestNonPodRelocation();
  TestComponentAlignment();
  TestChunkReclamation();
  TestAddRemoveOverwrite();
  TestStructuralMutationDuringIteration();
  TestCreateBatchFillsColumnRuns();
  TestCreateBatchReusesSlotsAndSkipsEmpty();
  if (g_failures) {
    std::fprintf(stderr, "ecs_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("ecs_test: ok");
  return 0;
}
