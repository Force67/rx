#include "ecs/world.h"

#include <cstdio>

namespace {

struct Position {
  float x = 0;
  float y = 0;
  float z = 0;
};

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "ecs_test: FAIL: %s\n", message);
  ++failures;
}

void TestWorldStatsTrackStoragePressure() {
  rx::ecs::World world;
  rx::ecs::Entity first = world.Create();
  rx::ecs::Entity second = world.Create();
  world.Add(first, Position{});
  world.Add(second, Position{});

  rx::ecs::World::Stats stats = world.stats();
  Check(stats.entity_count == 2, "live entity count");
  Check(stats.entity_slots == 2, "entity slots");
  Check(stats.archetype_count == 2, "empty and position archetypes");
  Check(stats.live_component_bytes == 2 * sizeof(Position), "live component bytes");
  Check(stats.component_capacity_bytes >= stats.live_component_bytes,
        "component capacity covers live bytes");

  world.Destroy(first);
  stats = world.stats();
  Check(stats.entity_count == 1, "destroy updates live count");
  Check(stats.entity_slots == 2, "destroyed slot remains reusable");
  Check(stats.live_component_bytes == sizeof(Position), "destroy updates live bytes");

  world.Create();
  stats = world.stats();
  Check(stats.entity_count == 2, "create reuses free slot");
  Check(stats.entity_slots == 2, "reused slot does not grow storage");
}

}  // namespace

int main() {
  TestWorldStatsTrackStoragePressure();
  if (failures == 0) std::printf("ecs_test: ok\n");
  return failures == 0 ? 0 : 1;
}
