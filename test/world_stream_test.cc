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
#include "world/world_overlay.h"

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
  // Also bake a cheaper gameplay tier, so a cell has two payloads to choose
  // between and crossing the band actually changes what is resident.
  bool tiered = false;
};

constexpr u32 kProxyEntitiesPerCell = 2;

base::Vector<u8> BakeGameplay(u64 cell, const BakeOptions& options,
                              Tier tier = Tier::kStandard) {
  u32 stride = 0;
  u64 layout = 0;
  CHECK(RuntimeComponentLayout("Transform", &stride, &layout));
  CHECK(stride == sizeof(Transform));
  if (options.corrupt_layout_hash) layout ^= 1;

  const u32 rows = tier == Tier::kProxy ? kProxyEntitiesPerCell : kEntitiesPerCell;
  base::Vector<Transform> transforms;
  base::Vector<u64> ids;
  for (u32 i = 0; i < rows; ++i) {
    Transform transform;
    transform.position[0] = static_cast<f32>(cell);
    transform.position[1] = static_cast<f32>(i);
    transform.scale = 2.0f;
    transforms.push_back(transform);
    ids.push_back(EntityStableId(cell, i));
  }

  CellPayloadWriter writer(cell, Domain::kGameplay, tier);
  writer.set_bake_id(options.payload_bake_id);
  const u32 archetype = writer.BeginArchetype(rows);
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
      if (options.tiered) {
        index.AddPayload(cell, Domain::kGameplay, Tier::kProxy,
                         kProxyEntitiesPerCell * sizeof(Transform), kProxyEntitiesPerCell);
        pack.Add(CellPayloadPath("city", cell, Domain::kGameplay, Tier::kProxy),
                 BakeGameplay(cell, options, Tier::kProxy));
      }
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
  // Make the next `count` reads fail the way a briefly unmounted archive does:
  // the bytes are simply not there, and then later they are.
  void set_fail_next(u32 count) { fail_next_ = count; }

 private:
  void Finish(const CellLoadRequest& request) {
    CellLoadResult result;
    result.ticket = request.ticket;
    result.cell = request.cell;
    result.domain = request.domain;
    result.tier = request.tier;
    if (fail_next_ != 0) {
      --fail_next_;
      result.ok = false;
      result.error = "the archive was not there this time";
    } else {
      result.ok = map_.ReadPayload(vfs_, request.cell, request.domain, request.tier,
                                   &result.payload, &result.error);
    }
    ready_.push_back(std::move(result));
  }

  const WorldMap& map_;
  const Vfs& vfs_;
  base::Vector<CellLoadRequest> pending_;
  base::Vector<CellLoadResult> ready_;
  u32 begun_ = 0;
  u32 cancelled_ = 0;
  u32 fail_next_ = 0;
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

void TestBudgetedCommitIsNotResolvableHalfway(const fs::path& directory) {
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
  //
  // The rows that do exist are ordinary entities and a query walking the world
  // will see them, which is inherent to committing into a live world over
  // several frames. What is guaranteed is narrower and is what this checks: a
  // cell is not addressable by stable id until all of it is there.
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

void TestOverlayShapesTheCellOnTheWayIn(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "overlay.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  WorldOverlay overlay;
  overlay.set_bake_id(kBakeId);
  overlay.Destroy(EntityStableId(0, 1));
  overlay.Destroy(EntityStableId(0, 4));
  overlay.Destroy(InstanceStableId(0, 0));
  overlay.Move(EntityStableId(0, 2), {10, 20, 30}, {0, 0, 0, 1}, 5.0f);
  overlay.Move(InstanceStableId(0, 2), {40, 50, 60}, {0, 0, 0, 1}, 7.0f);

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy(/*rows_per_commit=*/2));  // deletions must survive the quanta
  CHECK(streamer.SetOverlay(&overlay));
  Settle(&streamer, &loader, At(32, 32));

  // The destroyed rows were never created, not created and then removed.
  CHECK(streamer.stats().entities == kEntitiesPerCell - 2);
  CHECK(world.entity_count() == kEntitiesPerCell - 2);
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(0, 1))));
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(0, 4))));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 0))));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 5))));

  // Survivors keep their baked transform ...
  const Transform* untouched = world.Get<Transform>(streamer.Resolve(EntityStableId(0, 5)));
  CHECK(untouched && untouched->position[1] == 5.0f);
  CHECK(untouched && untouched->scale == 2.0f);
  // ... and a moved one comes up where the save says, not where the bake does.
  const Transform* moved = world.Get<Transform>(streamer.Resolve(EntityStableId(0, 2)));
  CHECK(moved != nullptr);
  CHECK(moved && moved->position[0] == 10.0f);
  CHECK(moved && moved->position[1] == 20.0f);
  CHECK(moved && moved->scale == 5.0f);
  // The stable id is still the row's own, not the one it was copied over.
  const CellResident* resident = world.Get<CellResident>(streamer.Resolve(EntityStableId(0, 2)));
  CHECK(resident && resident->stable_id == EntityStableId(0, 2));

  // Instances follow the same rules.
  CHECK(streamer.stats().instances == kInstancesPerCell - 1);
  CHECK(streamer.FindInstance(InstanceStableId(0, 0)) == nullptr);
  const ResidentInstance* moved_instance = streamer.FindInstance(InstanceStableId(0, 2));
  CHECK(moved_instance != nullptr);
  CHECK(moved_instance && moved_instance->position.x == 40.0f);
  CHECK(moved_instance && moved_instance->scale == 7.0f);

  // A neighbouring cell the overlay says nothing about is untouched.
  WorldStreamPolicy wide = TestPolicy(/*rows_per_commit=*/2);
  wide[Domain::kGameplay].load_distance = 500;
  wide[Domain::kGameplay].retain_distance = 600;
  streamer.Configure(wide);
  Settle(&streamer, &loader, At(32, 32));
  CHECK(streamer.stats().entities == (kEntitiesPerCell - 2) + 3 * kEntitiesPerCell);
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(1, 1))));

  // The deletions survive a full unload and reload: the overlay is the record,
  // the resident cell is not.
  Settle(&streamer, &loader, At(5000, 5000));
  CHECK(world.entity_count() == 0);
  Settle(&streamer, &loader, At(32, 32));
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(0, 1))));
  const Transform* removed = world.Get<Transform>(streamer.Resolve(EntityStableId(0, 2)));
  CHECK(removed && removed->position[0] == 10.0f);
}

// An overlay recorded against a different cook names different rows. Applying
// it would not fail: it would delete and move whatever now carries those ids.
void TestOverlayFromAnotherBakeIsRefused(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "mismatch.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy());

  WorldOverlay stale;
  stale.set_bake_id(kBakeId + 1);
  stale.Destroy(EntityStableId(0, 1));
  CHECK(!streamer.SetOverlay(&stale));
  CHECK(!streamer.errors().empty());

  // Refused means not applied, not applied-anyway-with-a-warning.
  Settle(&streamer, &loader, At(32, 32));
  CHECK(streamer.stats().entities == kEntitiesPerCell);
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 1))));

  // The matching one is taken, and so is an unkeyed one built in memory.
  WorldOverlay matching;
  matching.set_bake_id(kBakeId);
  CHECK(streamer.SetOverlay(&matching));
  WorldOverlay unkeyed;
  CHECK(streamer.SetOverlay(&unkeyed));
  CHECK(streamer.SetOverlay(nullptr));
}

void TestOverlayThatDeletesEverythingLeavesNothing(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "wipe.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  WorldOverlay overlay;
  overlay.set_bake_id(kBakeId);
  for (u32 i = 0; i < kEntitiesPerCell; ++i) overlay.Destroy(EntityStableId(0, i));
  for (u32 i = 0; i < kInstancesPerCell; ++i) overlay.Destroy(InstanceStableId(0, i));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy(/*rows_per_commit=*/2));
  CHECK(streamer.SetOverlay(&overlay));
  Settle(&streamer, &loader, At(32, 32));

  // An emptied cell still becomes resident: it exists, it just holds nothing.
  // Leaving it stuck mid-commit instead would wedge the plan.
  CHECK(streamer.stats().resident == 2);
  CHECK(streamer.stats().pending == 0);
  CHECK(streamer.stats().entities == 0);
  CHECK(streamer.stats().instances == 0);
  CHECK(world.entity_count() == 0);
  CHECK(streamer.errors().empty());

  Settle(&streamer, &loader, At(5000, 5000));
  CHECK(streamer.stats().resident == 0);
  CHECK(streamer.stats().pending == 0);
}

void TestClaimSet() {
  ClaimSet claims;
  CHECK(claims.empty());
  const u64 hard = claims.Add({/*owner=*/1,
                               /*cell=*/2,
                               DomainMask(Domain::kGameplay),
                               ClaimKind::kHard,
                               /*expires_at_tick=*/0,
                               /*full_detail=*/false,
                               "the player is standing on it"});
  const u64 soft = claims.Add({7, 2, DomainMask(Domain::kRepresentation), ClaimKind::kSoft, 100,
                               false, "predicted camera movement"});
  claims.Add({7, 3, DomainMask(Domain::kGameplay), ClaimKind::kSpeculative, 0, false, "a guess"});
  CHECK(claims.size() == 3);

  CHECK(claims.Holds(2, DomainMask(Domain::kGameplay)));
  CHECK(!claims.Holds(2, DomainMask(Domain::kCollision)));
  CHECK(!claims.Holds(99, ~u32{0}));
  // Hard outranks soft outranks speculative outranks nothing.
  CHECK(claims.Priority(2, DomainMask(Domain::kGameplay)) >
        claims.Priority(2, DomainMask(Domain::kRepresentation)));
  CHECK(claims.Priority(2, DomainMask(Domain::kRepresentation)) >
        claims.Priority(3, DomainMask(Domain::kGameplay)));
  CHECK(claims.Priority(3, DomainMask(Domain::kGameplay)) > claims.Priority(99, ~u32{0}));

  // "Why is this cell still resident?" has an answer, most binding first.
  base::Vector<ResidencyClaim> why;
  claims.Explain(2, ~u32{0}, &why);
  CHECK(why.size() == 2);
  CHECK(why.size() == 2 && why[0].kind == ClaimKind::kHard);
  CHECK(why.size() == 2 && std::string(why[0].reason) == "the player is standing on it");

  // Pressure revokes the weak and never the hard.
  claims.set_weakest_honored(ClaimKind::kSoft);
  CHECK(claims.Holds(2, DomainMask(Domain::kRepresentation)));
  CHECK(!claims.Holds(3, DomainMask(Domain::kGameplay)));
  claims.set_weakest_honored(ClaimKind::kHard);
  CHECK(claims.Holds(2, DomainMask(Domain::kGameplay)));
  CHECK(!claims.Holds(2, DomainMask(Domain::kRepresentation)));
  // Raising the bar past hard is refused: a hard claim is not revocable.
  claims.set_weakest_honored(static_cast<ClaimKind>(0));
  CHECK(claims.weakest_honored() == ClaimKind::kHard);
  claims.set_weakest_honored(ClaimKind::kSpeculative);

  // A lease expires on the tick the host says, not on its own.
  CHECK(claims.Expire(99) == 0);
  CHECK(claims.Expire(100) == 1);
  CHECK(!claims.Remove(soft));  // already expired
  CHECK(claims.Remove(hard));
  CHECK(!claims.Remove(hard));  // handles are never reused
  CHECK(claims.RemoveOwner(7) == 1);
  CHECK(claims.empty());
}

void TestClaimKeepsACellResident(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "claims.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy());
  ClaimSet claims;
  streamer.SetClaims(&claims);

  // Nobody is anywhere near cell 3, and its gameplay is claimed.
  const u64 handle = claims.Add({/*owner=*/42, /*cell=*/3, DomainMask(Domain::kGameplay),
                                 ClaimKind::kHard, 0, false, "a quest runs here"});
  const WorldStreamObservation away = At(5000, 5000);
  Settle(&streamer, &loader, away);

  CHECK(streamer.stats().entities == kEntitiesPerCell);
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 0))));
  // Exactly the claimed cell, and exactly the claimed domain: the neighbours
  // stay out, and so does cell 3's own representation payload.
  CHECK(streamer.stats().resident == 1);
  CHECK(streamer.stats().instances == 0);
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(2, 0))));

  // Revoking under pressure must not drop a hard claim.
  claims.set_weakest_honored(ClaimKind::kHard);
  Settle(&streamer, &loader, away);
  CHECK(streamer.stats().entities == kEntitiesPerCell);
  claims.set_weakest_honored(ClaimKind::kSpeculative);

  // Release the lease and the cell goes, with no observer having moved.
  CHECK(claims.Remove(handle));
  Settle(&streamer, &loader, away);
  CHECK(streamer.stats().entities == 0);
  CHECK(streamer.stats().resident == 0);
  CHECK(world.entity_count() == 0);

  // A soft claim behaves the same until the host raises the bar under pressure.
  claims.Add({42, 1, DomainMask(Domain::kGameplay), ClaimKind::kSoft, 0, false, "likely encounter"});
  Settle(&streamer, &loader, away);
  CHECK(streamer.stats().entities == kEntitiesPerCell);
  claims.set_weakest_honored(ClaimKind::kHard);
  Settle(&streamer, &loader, away);
  CHECK(streamer.stats().entities == 0);
}

// A cell that enters the bubble far away comes in cheap, and refines when the
// observer closes on it. Without this the near tier is unreachable: a cell
// always enters at roughly the load radius, which is by definition the far
// band, and would keep that tier for as long as it stayed resident.
void TestTierRefinesWhenTheObserverCloses(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.tiered = true;
  MountWorld(directory / "tiers.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kGameplay].load_distance = 200;
  policy[Domain::kGameplay].retain_distance = 240;
  // Cells are 64 m: standing in the middle of one puts its neighbours 32 m away
  // and the diagonal one 45 m, so a 20 m band means only the cell underfoot is
  // near, and the margin lets it drift to 40 m before dropping back.
  policy[Domain::kGameplay].full_tier_distance = 20;
  policy[Domain::kGameplay].tier_hysteresis = 2.0f;
  policy[Domain::kGameplay].near_tier = Tier::kFull;
  policy[Domain::kGameplay].far_tier = Tier::kProxy;
  policy[Domain::kRepresentation].load_distance = 0;  // one domain at a time
  streamer.Configure(policy);

  // Standing in cell 0, far from cell 3: one at the near tier, one at the far.
  Settle(&streamer, &loader, At(32, 32));
  CHECK(streamer.errors().empty());
  CHECK(streamer.stats().entities == kEntitiesPerCell + 3 * kProxyEntitiesPerCell);
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 5))));   // full has six rows
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));  // proxy has two
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 1))));

  // Walk into cell 3. It refines to the full tier without the observer ever
  // leaving its retain radius, so the reload is guaranteed to happen - but it
  // is a reload: the proxy rows are destroyed before the full ones arrive, and
  // for a domain carrying behavior that gap is a visible despawn. Hence
  // DefaultWorldStreamPolicy leaving gameplay on one tier.
  Settle(&streamer, &loader, At(96, 96));
  CHECK(streamer.errors().empty());
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
  CHECK(streamer.stats().entities == kEntitiesPerCell + 3 * kProxyEntitiesPerCell);

  // Step past the band edge but inside the hysteresis margin: the cell keeps
  // the tier it has rather than reloading on a wobble. Cell 3 starts at x = 64,
  // so this stands 30 m from it - outside the 20 m band, inside the 40 m one.
  Settle(&streamer, &loader, At(34, 96));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));

  // Far enough to clear the margin: back to the cheap tier.
  Settle(&streamer, &loader, At(-60, 96));
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 1))));
  CHECK(streamer.errors().empty());
}

// A world baked at one tier per domain must not churn: both bands resolve to
// the same payload, so nothing about the region changes as the observer moves
// and the cell is never reloaded.
void TestSingleTierWorldNeverReloads(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "single.rxp", &vfs);  // gameplay at kStandard only
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kGameplay].load_distance = 200;
  policy[Domain::kGameplay].retain_distance = 240;
  policy[Domain::kGameplay].full_tier_distance = 40;
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);

  Settle(&streamer, &loader, At(-60, 96));
  const Entity before = streamer.Resolve(EntityStableId(3, 0));
  CHECK(world.IsAlive(before));
  const u32 reads = loader.begun();

  // Cross the band in both directions. Nothing reloads, so nothing is re-read.
  Settle(&streamer, &loader, At(96, 96));
  Settle(&streamer, &loader, At(-60, 96));
  CHECK(streamer.Resolve(EntityStableId(3, 0)) == before);
  CHECK(loader.begun() == reads);
}

// A cook error is deterministic: the same payload fails the same way every
// time. Left alone the planner retries it for as long as the cell is in range,
// re-reading and re-decoding broken bytes forever.
void TestPersistentFailuresStopRetrying(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.corrupt_layout_hash = true;
  MountWorld(directory / "broken.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kRepresentation].load_distance = 0;  // only the broken domain
  streamer.Configure(policy);

  // Long enough for many retry intervals (the plan's default is 30 ticks).
  Settle(&streamer, &loader, At(32, 32), 400);
  CHECK(streamer.stats().entities == 0);
  CHECK(!streamer.errors().empty());
  // The latch is 3 per (cell, domain); the bubble here covers one cell, whose
  // representation payload is fine and loads once. Anything beyond that plus
  // slack means the retry loop is still running.
  const u32 attempts = loader.begun();
  if (attempts > 8) {
    std::fprintf(stderr, "FAIL: a broken cell was read %u times in 400 ticks\n", attempts);
    ++g_failures;
  }

  // And it stays stopped rather than resuming on a later tick.
  Settle(&streamer, &loader, At(32, 32), 200);
  CHECK(loader.begun() == attempts);
  CHECK(streamer.stats().pending == 0);
}

// Teardown is budgeted on both paths. A cancel is the observer moving fast,
// which is exactly when a whole cell destroyed in one frame would show.
void TestTeardownRespectsTheBudget(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "teardown.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy(/*rows_per_commit=*/2));
  Settle(&streamer, &loader, At(32, 32));
  CHECK(streamer.stats().entities == kEntitiesPerCell);

  const WorldStreamObservation away = At(1000, 1000);
  u32 previous = streamer.stats().entities;
  u32 ticks_spent = 0;
  for (u32 i = 0; i < 40 && streamer.stats().entities > 0; ++i) {
    loader.CompleteAll();
    Tick(&streamer, away, 1);
    const u32 now = streamer.stats().entities;
    if (now < previous) {
      CHECK(previous - now <= 2);  // never more than one quantum in a tick
      ++ticks_spent;
    }
    previous = now;
  }
  CHECK(streamer.stats().entities == 0);
  CHECK(ticks_spent > 1);  // six entities at two per tick cannot be one tick
  // One retiring cell per domain here. The budget is per retiring cell, so a
  // tick with several of them destroys several quanta; that is the planner's
  // pending cap to bound, not this one.
  CHECK(world.entity_count() == 0);
}

// A claim admits a cell; it does not decide the cell's detail. If it did, the
// claim's own source stands at the cell's middle, so taking or dropping a lease
// would evict and rebuild a cell that was already resident and correct - and a
// lease that is re-issued periodically would put the cell on a treadmill.
void TestClaimDoesNotChangeAResidentCellsTier(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.tiered = true;
  MountWorld(directory / "claimtier.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kGameplay].load_distance = 500;
  policy[Domain::kGameplay].retain_distance = 600;
  policy[Domain::kGameplay].full_tier_distance = 20;
  policy[Domain::kGameplay].near_tier = Tier::kFull;
  policy[Domain::kGameplay].far_tier = Tier::kProxy;
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);
  ClaimSet claims;
  streamer.SetClaims(&claims);

  // Cell 3 is far away, so it is resident at the cheap tier.
  Settle(&streamer, &loader, At(32, 32));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 1))));
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
  const Entity before = streamer.Resolve(EntityStableId(3, 1));
  const u32 reads = loader.begun();

  // Claim it, then drop the claim. Neither may touch the cell: same handle,
  // same tier, no re-read.
  const u64 handle = claims.Add({1, 3, DomainMask(Domain::kGameplay), ClaimKind::kSoft, 0, false,
                                 "a guess"});
  Settle(&streamer, &loader, At(32, 32));
  CHECK(streamer.Resolve(EntityStableId(3, 1)) == before);
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
  CHECK(loader.begun() == reads);

  CHECK(claims.Remove(handle));
  Settle(&streamer, &loader, At(32, 32));
  CHECK(streamer.Resolve(EntityStableId(3, 1)) == before);
  CHECK(loader.begun() == reads);

  // A claim that does want the detail says so, and then it is a reload.
  claims.Add({1, 3, DomainMask(Domain::kGameplay), ClaimKind::kHard, 0, /*full_detail=*/true,
              "a cutscene plays here"});
  Settle(&streamer, &loader, At(32, 32));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
  CHECK(loader.begun() > reads);

  // A claim on a cell nobody is near still loads it, at the cheap tier.
  claims.Clear();
  claims.Add({1, 3, DomainMask(Domain::kGameplay), ClaimKind::kSoft, 0, false, "keep it"});
  Settle(&streamer, &loader, At(5000, 5000));
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 1))));
  CHECK(!world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
}

// Suppression throttles a failing cell rather than removing it from the world
// forever: the streamer cannot tell a broken cook from an archive that was
// missing for a moment.
void TestSuppressionIsAThrottleNotABan(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.corrupt_layout_hash = true;
  MountWorld(directory / "throttle.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);

  const WorldStreamObservation inside = At(32, 32);
  Settle(&streamer, &loader, inside, 400);
  CHECK(streamer.stats().suppressed == 1);
  CHECK(streamer.stats().failed == 0);  // suppressed, not merely waiting
  const u32 throttled = loader.begun();

  // Past the retry interval it is offered again, fails again, and re-suppresses
  // rather than resuming the tight loop.
  Settle(&streamer, &loader, inside, 1800);
  const u32 after = loader.begun();
  CHECK(after > throttled);
  if (after - throttled > 4) {
    std::fprintf(stderr, "FAIL: a suppressed cell was retried %u times in 1800 ticks\n",
                 after - throttled);
    ++g_failures;
  }
  CHECK(streamer.stats().suppressed == 1);

  // And a host that has just remounted its archives can say so.
  streamer.ClearFailures();
  CHECK(streamer.stats().suppressed == 0);
  Settle(&streamer, &loader, inside, 5);
  CHECK(loader.begun() > after);
  CHECK(streamer.stats().entities == 0);  // still broken, so still nothing
}

// Every other test in this file uses a cell with one archetype and one column,
// which leaves the indexing that makes several of either work unexercised:
// resolved[next_archetype], columns[column_first + c], and the transform column
// an overlay move rewrites. A real cell has neither shape.
//
// It is also the only cell here big enough to cross an ECS chunk, so it is what
// proves the bulk copy advances by the run it is handed instead of assuming one
// pointer covers the batch.
void TestManyArchetypesColumnsAndChunks(const fs::path& directory) {
  fs::create_directories(directory);
  using rx::scene::Guid;
  using rx::scene::SpawnedFrom;

  // 16 KiB chunks; {CellResident, Transform, Guid} is 48 bytes, so ~341 rows
  // fit and 900 spans three.
  constexpr u32 kWide = 900;
  u32 transform_stride = 0, guid_stride = 0, spawned_stride = 0;
  u64 transform_layout = 0, guid_layout = 0, spawned_layout = 0;
  CHECK(RuntimeComponentLayout("Transform", &transform_stride, &transform_layout));
  CHECK(RuntimeComponentLayout("Guid", &guid_stride, &guid_layout));
  CHECK(RuntimeComponentLayout("SpawnedFrom", &spawned_stride, &spawned_layout));

  base::Vector<Transform> wide_transforms;
  base::Vector<Guid> wide_guids;
  base::Vector<u64> wide_ids;
  for (u32 i = 0; i < kWide; ++i) {
    Transform transform;
    transform.position[0] = static_cast<f32>(i);
    transform.scale = 3.0f;
    wide_transforms.push_back(transform);
    wide_guids.push_back(Guid{1000 + i});
    wide_ids.push_back(i);
  }
  // A second archetype whose columns are in a different order and whose
  // Transform is not column zero.
  const SpawnedFrom spawned[2] = {{0x112233}, {0x445566}};
  const Transform narrow[2] = {{{7, 8, 9}, {0, 0, 0, 1}, 4.0f}, {{1, 2, 3}, {0, 0, 0, 1}, 5.0f}};
  const u64 narrow_ids[2] = {kWide, kWide + 1};

  CellPayloadWriter writer(0, Domain::kGameplay, Tier::kStandard);
  writer.set_bake_id(kBakeId);
  const u32 wide = writer.BeginArchetype(kWide);
  writer.AddColumn(wide, "Transform", transform_stride, transform_layout,
                   std::span<const u8>(reinterpret_cast<const u8*>(wide_transforms.data()),
                                       wide_transforms.size() * sizeof(Transform)));
  writer.AddColumn(wide, "Guid", guid_stride, guid_layout,
                   std::span<const u8>(reinterpret_cast<const u8*>(wide_guids.data()),
                                       wide_guids.size() * sizeof(Guid)));
  writer.SetStableIds(wide, std::span<const u64>(wide_ids.data(), wide_ids.size()));

  const u32 second = writer.BeginArchetype(2);
  writer.AddColumn(second, "SpawnedFrom", spawned_stride, spawned_layout,
                   std::span<const u8>(reinterpret_cast<const u8*>(spawned), sizeof(spawned)));
  writer.AddColumn(second, "Transform", transform_stride, transform_layout,
                   std::span<const u8>(reinterpret_cast<const u8*>(narrow), sizeof(narrow)));
  writer.SetStableIds(second, std::span<const u64>(narrow_ids, 2));

  base::Vector<u8> payload;
  std::string error;
  CHECK(writer.Encode(&payload, &error));

  WorldIndexWriter index;
  index.set_bake_id(kBakeId);
  index.AddCell(0, {0, 0, 0}, {64, 32, 64}, 0, 0, kWide + 2);
  index.AddPayload(0, Domain::kGameplay, Tier::kStandard, kWide * 48, kWide + 2);
  base::Vector<u8> index_bytes;
  CHECK(index.Encode(&index_bytes, &error));

  PackWriter pack;
  pack.Add("city/city.rxworld", std::move(index_bytes));
  pack.Add(CellPayloadPath("city", 0, Domain::kGameplay, Tier::kStandard), std::move(payload));
  CHECK(pack.WriteTo((directory / "wide.rxp").string()));
  Vfs vfs;
  auto provider = rx::asset::MakePackFileProvider((directory / "wide.rxp").string());
  CHECK(provider != nullptr);
  if (!provider) return;
  vfs.Mount("world", std::move(provider));

  WorldMap map;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  // An overlay that deletes and moves rows in both archetypes, so the
  // row-by-row path and the transform-column lookup run on a payload where the
  // transform is not the first column.
  WorldOverlay overlay;
  overlay.set_bake_id(kBakeId);
  overlay.Destroy(5);
  overlay.Destroy(800);  // in a later chunk than row 5
  overlay.Move(400, {50, 60, 70}, {0, 0, 0, 1}, 9.0f);
  overlay.Move(kWide + 1, {11, 12, 13}, {0, 0, 0, 1}, 8.0f);

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy(/*rows_per_commit=*/250);  // several quanta per chunk
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);
  CHECK(streamer.SetOverlay(&overlay));
  Settle(&streamer, &loader, At(32, 32), 60);

  CHECK(streamer.errors().empty());
  for (const std::string& message : streamer.errors()) {
    std::fprintf(stderr, "  error: %s\n", message.c_str());
  }
  CHECK(streamer.stats().entities == kWide + 2 - 2);
  CHECK(world.entity_count() == kWide + 2 - 2);

  // Both columns of the wide archetype arrived, on rows either side of every
  // chunk boundary, and the two columns stayed paired.
  for (u32 i = 0; i < kWide; ++i) {
    if (i == 5 || i == 800) {
      CHECK(!world.IsAlive(streamer.Resolve(i)));
      continue;
    }
    const Entity entity = streamer.Resolve(i);
    if (!world.IsAlive(entity)) {
      std::fprintf(stderr, "FAIL: row %u did not materialize\n", i);
      ++g_failures;
      break;
    }
    const Transform* transform = world.Get<Transform>(entity);
    const Guid* guid = world.Get<Guid>(entity);
    if (!transform || !guid || guid->value != 1000 + i ||
        (i != 400 && transform->position[0] != static_cast<f32>(i))) {
      std::fprintf(stderr, "FAIL: row %u came back wrong\n", i);
      ++g_failures;
      break;
    }
  }
  // The moved row took the overlay's transform and kept its own guid.
  const Entity moved = streamer.Resolve(400);
  CHECK(world.Get<Transform>(moved)->position[0] == 50.0f);
  CHECK(world.Get<Transform>(moved)->scale == 9.0f);
  CHECK(world.Get<Guid>(moved)->value == 1400);

  // The second archetype: a different component set, and a Transform that is
  // not its first column, so a move that assumed column zero would write over
  // the SpawnedFrom instead.
  const Entity other = streamer.Resolve(kWide);
  CHECK(world.IsAlive(other));
  CHECK(world.Get<SpawnedFrom>(other) != nullptr &&
        world.Get<SpawnedFrom>(other)->prefab == 0x112233);
  CHECK(world.Get<Transform>(other) != nullptr && world.Get<Transform>(other)->scale == 4.0f);
  CHECK(world.Get<Guid>(other) == nullptr);  // not in this archetype
  const Entity moved_other = streamer.Resolve(kWide + 1);
  CHECK(world.Get<SpawnedFrom>(moved_other) != nullptr &&
        world.Get<SpawnedFrom>(moved_other)->prefab == 0x445566);
  CHECK(world.Get<Transform>(moved_other) != nullptr &&
        world.Get<Transform>(moved_other)->position[0] == 11.0f);

  Settle(&streamer, &loader, At(5000, 5000), 60);
  CHECK(world.entity_count() == 0);
}

// Several observers in one tick: the merge has to fold them to one candidate
// per cell at the nearest distance, or a far observer's view of a cell decides
// its tier.
void TestManyObserversFoldToTheNearest(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.tiered = true;
  MountWorld(directory / "observers.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kGameplay].load_distance = 500;
  policy[Domain::kGameplay].retain_distance = 600;
  policy[Domain::kGameplay].full_tier_distance = 20;
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);

  // One observer far from cell 3, one standing in it. The near one has to win.
  const WorldStreamObservation observers[2] = {At(32, 32), At(96, 96)};
  for (u32 i = 0; i < 40; ++i) {
    loader.CompleteAll();
    streamer.Update(std::span<const WorldStreamObservation>(observers, 2));
  }
  CHECK(streamer.errors().empty());
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));  // full tier, six rows
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 5))));  // and so is cell 0
  // Reversing the order must not change the answer.
  const WorldStreamObservation reversed[2] = {At(96, 96), At(32, 32)};
  for (u32 i = 0; i < 10; ++i) {
    loader.CompleteAll();
    streamer.Update(std::span<const WorldStreamObservation>(reversed, 2));
  }
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(3, 5))));
}

// The payload of a cancelled generation arriving while a NEW generation of the
// same cell is in flight. The two differ in tier, so adopting the stale one is
// visible: the cell would publish the two rows of the proxy payload under the
// generation that asked for the six of the full one.
void TestStalePayloadIsNotAdoptedByANewGeneration(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.tiered = true;
  MountWorld(directory / "stalegen.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  loader.set_honor_cancel(false);  // the worker finishes anyway
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kGameplay].load_distance = 500;
  policy[Domain::kGameplay].retain_distance = 600;
  policy[Domain::kGameplay].full_tier_distance = 20;
  policy[Domain::kGameplay].near_tier = Tier::kFull;
  policy[Domain::kGameplay].far_tier = Tier::kProxy;
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);

  // Approach cell 0 from the far corner, so it is the nearest cell and the
  // first one a prepare slot goes to. A proxy-tier read starts, and is left in
  // flight.
  Tick(&streamer, At(-300, -300), 1);
  CHECK(loader.pending() > 0);

  // Walk into cell 0. The band changes, so the in-flight generation is
  // cancelled and a full-tier one takes its place - with the proxy read still
  // sitting unanswered in the loader.
  const WorldStreamObservation inside = At(32, 32);
  Tick(&streamer, inside, 8);
  CHECK(loader.cancelled() > 0);
  CHECK(loader.pending() >= 2);

  // Now everything arrives at once. The stale proxy payload must be dropped on
  // its ticket rather than adopted by the generation that asked for the full
  // one, which would publish two rows where six belong.
  Settle(&streamer, &loader, inside, 40);
  CHECK(streamer.errors().empty());
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 5))));  // the full tier's sixth row
  for (u32 i = 0; i < kEntitiesPerCell; ++i) {
    CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, i))));
  }
}

// A cell that fails once and then reads cleanly must not carry the failure
// forward: three unrelated transient failures over a session would otherwise
// suppress a cell whose cook is fine.
void TestATransientFailureIsForgottenOnSuccess(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "transient.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  WorldStreamPolicy policy = TestPolicy();
  policy[Domain::kRepresentation].load_distance = 0;
  streamer.Configure(policy);

  const WorldStreamObservation inside = At(32, 32);
  // Two failures, then let it read.
  loader.set_fail_next(2);
  Settle(&streamer, &loader, inside, 200);
  CHECK(streamer.stats().entities == kEntitiesPerCell);
  CHECK(streamer.stats().failed == 0);     // the tally was cleared by the read
  CHECK(streamer.stats().suppressed == 0);
  CHECK(streamer.errors().size() == 2);

  // Two more failures later still must not tip it over, because the successful
  // read in between reset the count.
  Settle(&streamer, &loader, At(5000, 5000), 40);
  loader.set_fail_next(2);
  Settle(&streamer, &loader, inside, 200);
  CHECK(streamer.stats().suppressed == 0);
  CHECK(streamer.stats().entities == kEntitiesPerCell);
}

// The handle a stable id resolves to is checked, not trusted: a game that
// destroys a streamed entity itself leaves the record behind until the cell
// unloads, and handing that handle out is precisely the dangling reference
// stable ids exist to prevent.
void TestResolveRefusesAnEntityTheGameDestroyed(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "destroyed.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  streamer.Configure(TestPolicy());
  Settle(&streamer, &loader, At(32, 32));

  const Entity entity = streamer.Resolve(EntityStableId(0, 2));
  CHECK(world.IsAlive(entity));
  world.Destroy(entity);
  CHECK(streamer.Resolve(EntityStableId(0, 2)) == Entity{});
  // Its neighbours are untouched, and the cell still tears down cleanly.
  CHECK(world.IsAlive(streamer.Resolve(EntityStableId(0, 3))));
  Settle(&streamer, &loader, At(5000, 5000));
  CHECK(world.entity_count() == 0);
}

// The shipped defaults have to produce a world that actually streams, or every
// game starts by discovering they do not.
void TestDefaultPolicyStreams(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  BakeOptions options;
  options.tiered = true;
  MountWorld(directory / "defaults.rxp", &vfs, options);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  rx::ecs::World world;
  QueuedLoader loader(map, vfs);
  WorldStreamer streamer(map, loader, world);
  // The cells are 64 m; scale the defaults down so their radii suit them.
  streamer.Configure(DefaultWorldStreamPolicy(0.5f));
  Settle(&streamer, &loader, At(32, 32), 60);
  CHECK(streamer.errors().empty());
  CHECK(streamer.stats().entities == 4 * kEntitiesPerCell);
  CHECK(streamer.stats().instances == 4 * kInstancesPerCell);

  // Gameplay is left on one tier by default, so walking around cannot make a
  // cell reload and take its entities with it.
  const u32 reads = loader.begun();
  Settle(&streamer, &loader, At(96, 96), 40);
  Settle(&streamer, &loader, At(32, 32), 40);
  CHECK(loader.begun() == reads);
  CHECK(streamer.stats().entities == 4 * kEntitiesPerCell);

  Settle(&streamer, &loader, At(5000, 5000), 80);
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
  TestBudgetedCommitIsNotResolvableHalfway(tmp / "budget");
  TestUnloadDuringCommitDestroysExactlyWhatWasMade(tmp / "interrupt");
  TestLateResultAfterCancelIsNotPublished(tmp / "late");
  TestSchemaDriftIsRefusedLoudly(tmp / "drift");
  TestOverlayShapesTheCellOnTheWayIn(tmp / "overlay");
  TestOverlayFromAnotherBakeIsRefused(tmp / "mismatch");
  TestOverlayThatDeletesEverythingLeavesNothing(tmp / "wipe");
  TestPromoteAnInstance(tmp / "promote");
  TestShutdownEmptiesTheWorld(tmp / "shutdown");
  TestTierRefinesWhenTheObserverCloses(tmp / "tiers");
  TestSingleTierWorldNeverReloads(tmp / "single");
  TestPersistentFailuresStopRetrying(tmp / "broken");
  TestTeardownRespectsTheBudget(tmp / "teardown");
  TestClaimSet();
  TestClaimKeepsACellResident(tmp / "claims");
  TestClaimDoesNotChangeAResidentCellsTier(tmp / "claimtier");
  TestSuppressionIsAThrottleNotABan(tmp / "throttle");
  TestManyArchetypesColumnsAndChunks(tmp / "wide");
  TestManyObserversFoldToTheNearest(tmp / "observers");
  TestStalePayloadIsNotAdoptedByANewGeneration(tmp / "stalegen");
  TestATransientFailureIsForgottenOnSuccess(tmp / "transient");
  TestResolveRefusesAnEntityTheGameDestroyed(tmp / "destroyed");
  TestDefaultPolicyStreams(tmp / "defaults");
  TestDeterministicAcrossRuns(tmp / "deterministic");

  fs::remove_all(tmp);
  if (g_failures) {
    std::fprintf(stderr, "world_stream_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_stream_test: ok");
  return 0;
}
