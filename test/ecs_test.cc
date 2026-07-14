// Acceptance test for the chunked ECS storage: spawn storms (the old
// exact-size column resize was O(n^2) and relocated rows bitwise), non-POD
// components with interior pointers (bitwise relocation would corrupt them),
// archetype transitions, swap-remove churn and chunk reclamation.
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

}  // namespace

int main() {
  TestSpawnStormAndIteration();
  TestNonPodRelocation();
  TestComponentAlignment();
  TestChunkReclamation();
  TestAddRemoveOverwrite();
  if (g_failures) {
    std::fprintf(stderr, "ecs_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("ecs_test: ok");
  return 0;
}
