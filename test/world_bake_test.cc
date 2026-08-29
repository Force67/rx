// End to end: an authored .rxscene through the rxworld baker into a .rxp, then
// mounted and streamed back as entities and instance pages. This is the only
// test that exercises the whole path the engine actually uses, including the
// tool, so it is the one that notices when the cook and the runtime drift
// apart in a way neither half's own tests can see.
//
// argv[1] is the rxworld binary (CMake passes $<TARGET_FILE:rxworld>).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "asset/pack.h"
#include "asset/vfs.h"
#include "ecs/world.h"
#include "scene/components.h"
#include "world/world_map.h"
#include "world/world_stream.h"

namespace {

namespace fs = std::filesystem;
using namespace rx::world;
using rx::asset::Vfs;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::ecs::Entity;
using rx::scene::Transform;
using rx::scene::WorldStreamObservation;

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// Two 32 m cells side by side on x. Cell A (x around 8) holds three plain
// entities and two renderables; cell B (x around 48) holds one of each. Only
// components this build registers, so the bake is strict.
constexpr const char* kScene = R"(rxscene 1

entity
Guid.value = 101
Transform.position = 8 0 8

entity
Guid.value = 102
Transform.position = 10 2 8

entity
Guid.value = 103
Transform.position = 12 0 9

entity
Transform.position = 9 1 12
Renderable.mesh = "meshes/rock.gltf"

entity
Transform.position = 11 1 13
Renderable.mesh = "meshes/rock.gltf"

entity
Guid.value = 201
Transform.position = 48 0 8

entity
Transform.position = 50 1 9
Renderable.mesh = "meshes/tree.gltf"
)";

void Tick(WorldStreamer* streamer, const WorldStreamObservation& observer, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    streamer->Update(std::span<const WorldStreamObservation>(&observer, 1));
  }
}

WorldStreamObservation At(f32 x, f32 z) {
  WorldStreamObservation observer;
  observer.position = {x, 0, z};
  observer.axes = rx::scene::kWorldStreamXZ;
  return observer;
}

WorldStreamPolicy Policy(f32 load, f32 retain) {
  WorldStreamPolicy policy;
  for (u32 i = 0; i < kDomainCount; ++i) policy.domains[i].load_distance = 0;
  for (Domain domain : {Domain::kGameplay, Domain::kRepresentation}) {
    policy[domain].load_distance = load;
    policy[domain].retain_distance = retain;
    policy[domain].full_tier_distance = load;
    policy[domain].rows_per_commit = 4096;
  }
  return policy;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: world_bake_test <path to rxworld>\n");
    return 2;
  }
  const std::string rxworld = argv[1];

  const fs::path tmp = fs::temp_directory_path() / "rx_world_bake_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);
  const fs::path scene = tmp / "town.rxscene";
  const fs::path archive = tmp / "town.rxp";
  {
    std::ofstream file(scene);
    file << kScene;
    CHECK(file.good());
  }

  const std::string command = "\"" + rxworld + "\" bake \"" + scene.string() + "\" \"" +
                              archive.string() + "\" --name town --cell-size 32";
  const int status = std::system(command.c_str());
  if (status != 0) {
    std::fprintf(stderr, "FAIL: rxworld bake exited %d\n", status);
    fs::remove_all(tmp);
    return 1;
  }
  CHECK(fs::exists(archive));

  // Everything past here is the engine reading what the tool wrote.
  Vfs vfs;
  auto provider = rx::asset::MakePackFileProvider(archive.string());
  CHECK(provider != nullptr);
  if (!provider) return 1;
  vfs.Mount("world", std::move(provider));

  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://town/town.rxworld", &error));
  CHECK(error.empty());
  CHECK(map.index().cells.size() == 2);
  CHECK(map.index().bake_id != 0);

  // Stable-id ranges partition the world, which is what lets an id be resolved
  // to a cell with nothing resident.
  rx::u32 total_ids = 0;
  for (const WorldCellRecord& cell : map.index().cells) total_ids += cell.stable_id_count;
  CHECK(total_ids == 7);

  rx::ecs::World world;
  auto loader = MakeArchiveCellLoader(map, vfs);
  WorldStreamer streamer(map, *loader, world);

  // A bubble that reaches only the first cell.
  streamer.Configure(Policy(8, 12));
  Tick(&streamer, At(10, 10), 30);
  CHECK(streamer.errors().empty());
  if (!streamer.errors().empty()) {
    for (const std::string& message : streamer.errors()) {
      std::fprintf(stderr, "  error: %s\n", message.c_str());
    }
  }
  // Three plain entities became ECS rows; the two renderables became instance
  // page rows and cost no entity at all.
  CHECK(streamer.stats().entities == 3);
  CHECK(streamer.stats().instances == 2);
  CHECK(world.entity_count() == 3);

  // The cooked column arrived verbatim: one of the three sits at y = 2.
  rx::u32 raised = 0;
  world.Each<Transform>([&](Entity, Transform& transform) {
    if (transform.position[1] == 2.0f) ++raised;
  });
  CHECK(raised == 1);

  const WorldCellRecord& first = map.index().cells[0];
  bool resolved_any = false;
  for (rx::u32 i = 0; i < first.stable_id_count; ++i) {
    const rx::u64 stable_id = first.stable_id_first + i;
    const Entity entity = streamer.Resolve(stable_id);
    const ResidentInstance* instance = streamer.FindInstance(stable_id);
    // Every id in a resident cell is one or the other, and never both.
    const bool live = world.IsAlive(entity);
    CHECK(live != (instance != nullptr));
    resolved_any |= live;
    if (instance) CHECK(instance->scale == 1.0f);
  }
  CHECK(resolved_any);

  // Widen the bubble and the second cell arrives without the first moving.
  streamer.Configure(Policy(200, 240));
  Tick(&streamer, At(10, 10), 30);
  CHECK(streamer.stats().entities == 4);
  CHECK(streamer.stats().instances == 3);
  CHECK(streamer.errors().empty());

  // Walk away and the world empties.
  Tick(&streamer, At(10000, 10000), 40);
  CHECK(streamer.stats().entities == 0);
  CHECK(streamer.stats().resident == 0);
  CHECK(world.entity_count() == 0);

  // Re-baking the same input must produce the same bake id, or every overlay
  // and save keyed to it is invalidated by a no-op rebuild.
  const fs::path second = tmp / "town2.rxp";
  const std::string again = "\"" + rxworld + "\" bake \"" + scene.string() + "\" \"" +
                            second.string() + "\" --name town --cell-size 32";
  CHECK(std::system(again.c_str()) == 0);
  Vfs second_vfs;
  auto second_provider = rx::asset::MakePackFileProvider(second.string());
  CHECK(second_provider != nullptr);
  if (second_provider) {
    second_vfs.Mount("world", std::move(second_provider));
    WorldMap second_map;
    CHECK(second_map.Load(second_vfs, "world://town/town.rxworld", &error));
    CHECK(second_map.index().bake_id == map.index().bake_id);
    CHECK(second_map.index().cells.size() == map.index().cells.size());
  }

  // A different cook of the same scene is a different bake, so its payloads
  // must not be readable through the first index.
  const fs::path other = tmp / "town3.rxp";
  const std::string different = "\"" + rxworld + "\" bake \"" + scene.string() + "\" \"" +
                                other.string() + "\" --name town --cell-size 64";
  CHECK(std::system(different.c_str()) == 0);
  Vfs other_vfs;
  auto other_provider = rx::asset::MakePackFileProvider(other.string());
  if (other_provider) {
    other_vfs.Mount("world", std::move(other_provider));
    WorldMap other_map;
    CHECK(other_map.Load(other_vfs, "world://town/town.rxworld", &error));
    CHECK(other_map.index().bake_id != map.index().bake_id);
  }

  fs::remove_all(tmp);
  if (g_failures) {
    std::fprintf(stderr, "world_bake_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_bake_test: ok");
  return 0;
}
