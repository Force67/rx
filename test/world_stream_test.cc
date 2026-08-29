// Streaming a baked world out of an archive and back out of memory: bulk
// materialization, budgeted commit and teardown, stable-id lifetime across an
// unload, static instances and their promotion, and the two ways this goes
// wrong quietly if nobody checks - a payload that arrives after its generation
// was cancelled, and a cook whose component layout no longer matches the build.
#include "world/world_stream.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "asset/pack.h"
#include "asset/vfs.h"
#include "ecs/world.h"
#include "scene/components.h"
#include "world/world_map.h"

namespace {

namespace fs = std::filesystem;
using namespace rx::world;
using rx::asset::PackWriter;
using rx::asset::Vfs;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;
using rx::Vec3;
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

constexpr u64 kBakeId = 0x5151515151515151ull;
constexpr f32 kCellSize = 64.0f;
constexpr u32 kEntitiesPerCell = 6;
constexpr u32 kInstancesPerCell = 3;

// Stable-id layout inside a cell's 100-wide range: entities low, instances at
// 50. Disjoint by construction, which is what the baker owes the index.
u64 EntityStableId(u64 cell, u32 i) { return cell * 100 + i; }
u64 InstanceStableId(u64 cell, u32 i) { return cell * 100 + 50 + i; }

struct BakeOptions {
  u64 payload_bake_id = kBakeId;
  bool corrupt_layout_hash = false;
  bool unknown_component = false;
  bool non_trivial_component = false;
};

base::Vector<u8> BakeGameplay(u64 cell, const BakeOptions& options) {
  u32 stride = 0;
  u64 layout = 0;
  CHECK(RuntimeComponentLayout("Transform", &stride, &layout));
  CHECK(stride == sizeof(Transform));
  if (options.corrupt_layout_hash) layout ^= 1;

  base::Vector<Transform> transforms;
  base::Vector<u64> ids;
  for (u32 i = 0; i < kEntitiesPerCell; ++i) {
    Transform transform;
    transform.position[0] = static_cast<f32>(cell);
    transform.position[1] = static_cast<f32>(i);
    transform.scale = 2.0f;
    transforms.push_back(transform);
    ids.push_back(EntityStableId(cell, i));
  }

  CellPayloadWriter writer(cell, Domain::kGameplay, Tier::kStandard);
  writer.set_bake_id(options.payload_bake_id);
  const u32 archetype = writer.BeginArchetype(kEntitiesPerCell);
  const char* name = options.unknown_component  ? "NoSuchComponentInThisBuild"
                     : options.non_trivial_component ? "Name"
                                                     : "Transform";
  writer.AddColumn(archetype, name, stride, layout,
                   std::span<const u8>(reinterpret_cast<const u8*>(transforms.data()),
                                       transforms.size() * sizeof(Transform)));
  writer.SetStableIds(archetype, std::span<const u64>(ids.data(), ids.size()));

  base::Vector<u8> bytes;
  std::string error;
  if (!writer.Encode(&bytes, &error)) {
    std::fprintf(stderr, "FAIL: baking gameplay: %s\n", error.c_str());
    ++g_failures;
  }
  return bytes;
}

base::Vector<u8> BakeRepresentation(u64 cell, const BakeOptions& options) {
  CellPayloadWriter writer(cell, Domain::kRepresentation, Tier::kFull);
  writer.set_bake_id(options.payload_bake_id);
  const u32 prototype = writer.AddPrototype("prop/rock");
  for (u32 i = 0; i < kInstancesPerCell; ++i) {
    writer.AddInstance(InstanceStableId(cell, i), prototype,
                       {static_cast<f32>(cell), static_cast<f32>(i), 0}, {0, 0, 0, 1}, 1.0f);
  }
  base::Vector<u8> bytes;
  std::string error;
  if (!writer.Encode(&bytes, &error)) {
    std::fprintf(stderr, "FAIL: baking representation: %s\n", error.c_str());
    ++g_failures;
  }
  return bytes;
}

// A 2x2 grid of 64 m cells on XZ, each with a gameplay and a representation
// payload, packed into a real .rxp and mounted at world://.
void MountWorld(const fs::path& archive, Vfs* vfs, const BakeOptions& options = {}) {
  WorldIndexWriter index;
  index.set_world_id(7);
  index.set_bake_id(kBakeId);
  index.set_grid(kCellSize, {0, 0, 0});
  PackWriter pack;
  for (u32 z = 0; z < 2; ++z) {
    for (u32 x = 0; x < 2; ++x) {
      const u64 cell = z * 2 + x;
      index.AddCell(cell, {x * kCellSize, 0, z * kCellSize},
                    {(x + 1) * kCellSize, 32, (z + 1) * kCellSize}, 0, cell * 100, 100);
      index.AddPayload(cell, Domain::kGameplay, Tier::kStandard,
                       kEntitiesPerCell * sizeof(Transform), kEntitiesPerCell);
      index.AddPayload(cell, Domain::kRepresentation, Tier::kFull, kInstancesPerCell * 48,
                       kInstancesPerCell);
      pack.Add(CellPayloadPath("city", cell, Domain::kGameplay, Tier::kStandard),
               BakeGameplay(cell, options));
      pack.Add(CellPayloadPath("city", cell, Domain::kRepresentation, Tier::kFull),
               BakeRepresentation(cell, options));
    }
  }
  base::Vector<u8> index_bytes;
  std::string error;
  CHECK(index.Encode(&index_bytes, &error));
  pack.Add("city/city.rxworld", std::move(index_bytes));
  CHECK(pack.WriteTo(archive.string()));

  auto provider = rx::asset::MakePackFileProvider(archive.string());
  CHECK(provider != nullptr);
  if (provider) vfs->Mount("world", std::move(provider));
}

WorldStreamPolicy TestPolicy(u32 rows_per_commit = 4096) {
  WorldStreamPolicy policy;
  for (u32 i = 0; i < kDomainCount; ++i) {
    DomainStreamPolicy& domain = policy.domains[i];
    domain.load_distance = 0;
    domain.rows_per_commit = rows_per_commit;
  }
  policy[Domain::kGameplay].load_distance = 16;
  policy[Domain::kGameplay].retain_distance = 24;
  policy[Domain::kGameplay].full_tier_distance = 16;
  policy[Domain::kRepresentation].load_distance = 16;
  policy[Domain::kRepresentation].retain_distance = 24;
  policy[Domain::kRepresentation].full_tier_distance = 16;
  return policy;
}

WorldStreamObservation At(f32 x, f32 z) {
  WorldStreamObservation observer;
  observer.position = {x, 0, z};
  observer.axes = rx::scene::kWorldStreamXZ;
  return observer;
}

void Tick(WorldStreamer* streamer, const WorldStreamObservation& observer, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    streamer->Update(std::span<const WorldStreamObservation>(&observer, 1));
  }
}

// A loader the test drives by hand: requests queue until Complete is called, so
// completions can arrive late, out of order, or after their generation is gone.
class QueuedLoader final : public CellLoader {
 public:
  QueuedLoader(const WorldMap& map, const Vfs& vfs) : map_(map), vfs_(vfs) {}

  void Begin(const CellLoadRequest& request) override {
    ++begun_;
    pending_.push_back(request);
  }

  void Cancel(rx::scene::WorldStreamTicket ticket) override {
    ++cancelled_;
    if (!honor_cancel_) return;
    for (size_t i = 0; i < pending_.size();) {
      if (pending_[i].ticket == ticket) {
        pending_.erase(pending_.begin() + i);
      } else {
        ++i;
      }
    }
  }

  void Poll(base::Vector<CellLoadResult>* out) override {
    for (CellLoadResult& result : ready_) out->push_back(std::move(result));
    ready_.clear();
  }

  // Moves every queued request to the ready list, reading the real payload.
  void CompleteAll() {
    for (const CellLoadRequest& request : pending_) Finish(request);
    pending_.clear();
  }

  size_t pending() const { return pending_.size(); }
  u32 begun() const { return begun_; }
  u32 cancelled() const { return cancelled_; }
  void set_honor_cancel(bool honor) { honor_cancel_ = honor; }

 private:
  void Finish(const CellLoadRequest& request) {
    CellLoadResult result;
    result.ticket = request.ticket;
    result.cell = request.cell;
    result.domain = request.domain;
    result.tier = request.tier;
    result.ok = map_.ReadPayload(vfs_, request.cell, request.domain, request.tier, &result.payload,
                                 &result.error);
    ready_.push_back(std::move(result));
  }

  const WorldMap& map_;
  const Vfs& vfs_;
  base::Vector<CellLoadRequest> pending_;
  base::Vector<CellLoadResult> ready_;
  u32 begun_ = 0;
  u32 cancelled_ = 0;
  bool honor_cancel_ = false;
};

// Drives the streamer until nothing is in flight, completing loads each tick.
void Settle(WorldStreamer* streamer, QueuedLoader* loader, const WorldStreamObservation& observer,
            u32 ticks = 40) {
  for (u32 i = 0; i < ticks; ++i) {
    loader->CompleteAll();
    Tick(streamer, observer, 1);
  }
}

void TestStreamInAndOut(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "world.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy());

  const WorldStreamObservation inside = At(32, 32);  // the middle of cell 0
  Settle(&streamer, &loader, inside);

  // Cell 0 only: the 16 m bubble does not reach its neighbours.
  WorldStreamerStats stats = streamer.stats();
  CHECK(stats.resident == 2);  // gameplay and representation of one cell
  CHECK(stats.pending == 0);
  CHECK(stats.entities == kEntitiesPerCell);
  CHECK(stats.instances == kInstancesPerCell);
  CHECK(streamer.errors().empty());
  CHECK(world.entity_count() == kEntitiesPerCell);

  // The columns arrived verbatim, and every entity knows where it came from.
  const Entity entity = streamer.Resolve(EntityStableId(0, 3));
  CHECK(world.IsAlive(entity));
  const Transform* transform = world.Get<Transform>(entity);
  CHECK(transform != nullptr);
  CHECK(transform && transform->position[0] == 0.0f);
  CHECK(transform && transform->position[1] == 3.0f);
  CHECK(transform && transform->scale == 2.0f);
  const CellResident* resident = world.Get<CellResident>(entity);
  CHECK(resident != nullptr);
  CHECK(resident && resident->stable_id == EntityStableId(0, 3));
  CHECK(resident && resident->cell == 0);

  // An id belonging to a cell that was never loaded resolves to nothing rather
  // than to whatever entity happens to sit at that index.
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(3, 0))));
  CHECK(!world.IsAlive(streamer.Resolve(999999)));

  // Static decoration is resident without costing an ECS row.
  CHECK(streamer.Instances(0).size() == kInstancesPerCell);
  const ResidentInstance* instance = streamer.FindInstance(InstanceStableId(0, 1));
  CHECK(instance != nullptr);
  CHECK(instance && instance->position.y == 1.0f);
  CHECK(streamer.FindInstance(InstanceStableId(3, 0)) == nullptr);

  // Walk out of the bubble: everything retires and the ECS goes back to empty.
  const WorldStreamObservation away = At(1000, 1000);
  Settle(&streamer, &loader, away);
  stats = streamer.stats();
  CHECK(stats.resident == 0);
  CHECK(stats.pending == 0);
  CHECK(stats.entities == 0);
  CHECK(world.entity_count() == 0);
  CHECK(!world.IsAlive(entity));
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(0, 3))));
  CHECK(streamer.Instances(0).empty());

  // Walk back in: the same stable id resolves again, to a different handle.
  Settle(&streamer, &loader, inside);
  const Entity reloaded = streamer.Resolve(EntityStableId(0, 3));
  CHECK(world.IsAlive(reloaded));
  CHECK(!(reloaded == entity));  // a reused index, but never the same generation
  CHECK(!world.IsAlive(entity));
  const Transform* reloaded_transform = world.Get<Transform>(reloaded);
  CHECK(reloaded_transform && reloaded_transform->position[1] == 3.0f);
}

void TestWideBubbleLoadsEveryCell(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "wide.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kGameplay].load_distance = 500;
  policy[Domain::kGameplay].retain_distance = 600;
  // Representation stays narrow: the two domains must not move together.
  streamer.Configure(policy);

  Settle(&streamer, &loader, At(32, 32));
  const WorldStreamerStats stats = streamer.stats();
  CHECK(stats.entities == 4 * kEntitiesPerCell);
  CHECK(stats.instances == kInstancesPerCell);  // still only cell 0
  CHECK(stats.resident == 5);                   // four gameplay, one representation
  CHECK(streamer.errors().empty());

  for (u64 cell = 0; cell < 4; ++cell) {
    CHECK(world.IsAlive(streamer.Resolve(EntityStableId(cell, 0))));
  }
}

void TestBudgetedCommitIsNotObservableHalfway(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "budget.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy(/*rows_per_commit=*/2));

  const WorldStreamObservation inside = At(32, 32);
  // Load, then commit two rows at a time. Until the cell is published, a stable
  // id must not resolve: half its rows exist and the rest never may.
  bool saw_partial = false;
  for (u32 i = 0; i < 40; ++i) {
    loader.CompleteAll();
    Tick(&streamer, inside, 1);
    const WorldStreamerStats stats = streamer.stats();
    if (stats.entities > 0 && stats.entities < kEntitiesPerCell) {
      saw_partial = true;
      CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(0, 0))));
    }
    if (stats.entities == kEntitiesPerCell && stats.pending == 0) break;
  }
  CHECK(saw_partial);
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 0))));
  CHECK(streamer.stats().entities == kEntitiesPerCell);
}

void TestUnloadDuringCommitDestroysExactlyWhatWasMade(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "interrupt.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  // An unrelated entity: teardown must not touch anything the streamer did not
  // create.
  const Entity bystander = world.Create();
  world.Add(bystander, Transform{});
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy(/*rows_per_commit=*/2));

  const WorldStreamObservation inside = At(32, 32);
  for (u32 i = 0; i < 40 && streamer.stats().entities == 0; ++i) {
    loader.CompleteAll();
    Tick(&streamer, inside, 1);
  }
  const u32 partial = streamer.stats().entities;
  CHECK(partial > 0);
  CHECK(partial < kEntitiesPerCell);

  // Leave while the cell is mid-commit.
  const WorldStreamObservation away = At(1000, 1000);
  Settle(&streamer, &loader, away);
  CHECK(streamer.stats().entities == 0);
  CHECK(streamer.stats().resident == 0);
  CHECK(streamer.stats().pending == 0);
  CHECK(world.entity_count() == 1);  // the bystander, and only the bystander
  CHECK(world.IsAlive(bystander));
}

void TestLateResultAfterCancelIsNotPublished(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "late.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  // The worst case: a worker that finishes the read anyway after being told to
  // stop, and hands the bytes over on a later tick.
  loader.set_honor_cancel(false);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy());

  const WorldStreamObservation inside = At(32, 32);
  Tick(&streamer, inside, 1);  // prepares, nothing completed
  CHECK(loader.pending() > 0);
  CHECK(streamer.stats().pending > 0);

  // Leave before anything arrives: the plan cancels the in-flight generations.
  const WorldStreamObservation away = At(1000, 1000);
  Tick(&streamer, away, 2);
  CHECK(loader.cancelled() > 0);
  CHECK(streamer.stats().pending == 0);

  // Now the worker delivers. Nothing may adopt it.
  loader.CompleteAll();
  Tick(&streamer, away, 2);
  CHECK(streamer.stats().entities == 0);
  CHECK(streamer.stats().resident == 0);
  CHECK(world.entity_count() == 0);
  CHECK(streamer.errors().empty());  // a stale result is not an error, just stale

  // And a fresh generation for the same cell must build from its own read, not
  // from the one that was already sitting in the queue.
  const u32 begun_before = loader.begun();
  Settle(&streamer, &loader, inside);
  CHECK(loader.begun() > begun_before);
  CHECK(streamer.stats().entities == kEntitiesPerCell);
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 0))));
}

void TestSchemaDriftIsRefusedLoudly(const fs::path& directory) {
  struct Case {
    const char* name;
    BakeOptions options;
    const char* expect;
  };
  const Case cases[] = {
      {"hash", [] { BakeOptions o; o.corrupt_layout_hash = true; return o; }(), "field layout"},
      {"unknown", [] { BakeOptions o; o.unknown_component = true; return o; }(), "not registered"},
      {"nontrivial", [] { BakeOptions o; o.non_trivial_component = true; return o; }(),
       "indirection"},
      {"stale", [] { BakeOptions o; o.payload_bake_id = kBakeId + 1; return o; }(), "baked by"},
  };

  for (const Case& test_case : cases) {
    const fs::path sub = directory / test_case.name;
    fs::create_directories(sub);
    Vfs vfs;
    MountWorld(sub / "drift.rxp", &vfs, test_case.options);
    WorldMap map;
    std::string error;
    CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

    rx::ecs::World world;
    QueuedLoader loader(map, vfs);
    WorldStreamer streamer(map, loader, world);
    streamer.Configure(TestPolicy());
    Settle(&streamer, &loader, At(32, 32), 10);

    // Not one entity, and a message that names the component or the bake.
    CHECK(streamer.stats().entities == 0);
    CHECK(world.entity_count() == 0);
    bool named = false;
    for (const std::string& message : streamer.errors()) {
      if (message.find(test_case.expect) != std::string::npos) named = true;
    }
    if (!named) {
      std::fprintf(stderr, "FAIL: %s drift produced no message mentioning '%s'\n", test_case.name,
                   test_case.expect);
      for (const std::string& message : streamer.errors()) {
        std::fprintf(stderr, "  had: %s\n", message.c_str());
      }
      ++g_failures;
    }
  }
}

void TestPromoteAnInstance(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "promote.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy());
  Settle(&streamer, &loader, At(32, 32));
  CHECK(world.entity_count() == kEntitiesPerCell);

  const u64 stable_id = InstanceStableId(0, 2);
  CHECK(!world.IsAlive(streamer.Resolve(stable_id)));  // a page row, not an entity yet

  const Entity promoted = streamer.Promote(stable_id);
  CHECK(world.IsAlive(promoted));
  CHECK(world.entity_count() == kEntitiesPerCell + 1);
  const Transform* transform = world.Get<Transform>(promoted);
  CHECK(transform != nullptr);
  CHECK(transform && transform->position[1] == 2.0f);
  const CellResident* resident = world.Get<CellResident>(promoted);
  CHECK(resident && resident->stable_id == stable_id);
  // It resolves under the same id it had as an instance.
  CHECK(streamer.Resolve(stable_id) == promoted);
  // ... and promoting twice is a no-op rather than a second entity.
  CHECK(!world.IsAlive(streamer.Promote(stable_id)));
  CHECK(world.entity_count() == kEntitiesPerCell + 1);
  CHECK(streamer.Promote(InstanceStableId(3, 0)) == Entity{});  // cell not resident

  // A promoted entity is still the cell's, so unloading takes it too.
  Settle(&streamer, &loader, At(1000, 1000));
  CHECK(world.entity_count() == 0);
  CHECK(!world.IsAlive(promoted));
}

void TestShutdownEmptiesTheWorld(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "shutdown.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  {
    QueuedLoader loader(map, vfs);
    WorldStreamer streamer(map, loader, world);
    streamer.Configure(TestPolicy());
    Settle(&streamer, &loader, At(32, 32));
    CHECK(world.entity_count() == kEntitiesPerCell);
    // Destroying the streamer must leave nothing of its behind: the ecs::World
    // outlives it here, exactly as it does in a host that swaps worlds.
  }
  CHECK(world.entity_count() == 0);
}

void TestDeterministicAcrossRuns(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "deterministic.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  auto run = [&] {
    rx::ecs::World world;
    QueuedLoader loader(map, vfs);
    WorldStreamer streamer(map, loader, world);
    streamer.Configure(TestPolicy(/*rows_per_commit=*/2));
    base::Vector<u32> trace;
    const WorldStreamObservation inside = At(32, 32);
    for (u32 i = 0; i < 20; ++i) {
      loader.CompleteAll();
      Tick(&streamer, inside, 1);
      trace.push_back(streamer.stats().entities);
    }
    return trace;
  };

  // Same inputs, same tick-by-tick shape. The planner promises this; the shell
  // has to not break it.
  const base::Vector<u32> first = run();
  const base::Vector<u32> second = run();
  CHECK(first.size() == second.size());
  for (size_t i = 0; i < first.size() && i < second.size(); ++i) CHECK(first[i] == second[i]);
}

}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / "rx_world_stream_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  TestStreamInAndOut(tmp / "roundtrip");
  TestWideBubbleLoadsEveryCell(tmp / "wide");
  TestBudgetedCommitIsNotObservableHalfway(tmp / "budget");
  TestUnloadDuringCommitDestroysExactlyWhatWasMade(tmp / "interrupt");
  TestLateResultAfterCancelIsNotPublished(tmp / "late");
  TestSchemaDriftIsRefusedLoudly(tmp / "drift");
  TestPromoteAnInstance(tmp / "promote");
  TestShutdownEmptiesTheWorld(tmp / "shutdown");
  TestDeterministicAcrossRuns(tmp / "deterministic");

  fs::remove_all(tmp);
  if (g_failures) {
    std::fprintf(stderr, "world_stream_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_stream_test: ok");
  return 0;
}
