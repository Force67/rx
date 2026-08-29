// The runtime side of the baked map: an index loaded out of a real .rxp,
// payloads read back through the Vfs, per-domain streaming bubbles, and the
// refusals that keep a stale or mismatched archive from being streamed.
#include "world/world_map.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "asset/pack.h"
#include "asset/vfs.h"

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

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

constexpr u64 kBakeId = 0xabcdef0123456789ull;
constexpr f32 kCellSize = 64.0f;

// A 2x2 grid on XZ. Cell (x, z) has id z * 2 + x and owns 100 stable ids.
// Every cell carries gameplay at standard and proxy, and representation at
// full; only cell 0 carries collision, so "the cook produced nothing here" is
// covered too.
u64 CellId(u32 x, u32 z) { return z * 2 + x; }

base::Vector<u8> BakeIndex() {
  WorldIndexWriter writer;
  writer.set_world_id(1);
  writer.set_bake_id(kBakeId);
  writer.set_grid(kCellSize, {0, 0, 0});
  for (u32 z = 0; z < 2; ++z) {
    for (u32 x = 0; x < 2; ++x) {
      const u64 id = CellId(x, z);
      writer.AddCell(id, {x * kCellSize, 0, z * kCellSize},
                     {(x + 1) * kCellSize, 32, (z + 1) * kCellSize}, 0, id * 100, 100);
      writer.AddPayload(id, Domain::kGameplay, Tier::kStandard, 1024, 8);
      writer.AddPayload(id, Domain::kGameplay, Tier::kProxy, 128, 1);
      writer.AddPayload(id, Domain::kRepresentation, Tier::kFull, 4096, 400);
    }
  }
  writer.AddPayload(0, Domain::kCollision, Tier::kStandard, 512, 1);

  base::Vector<u8> bytes;
  std::string error;
  if (!writer.Encode(&bytes, &error)) {
    std::fprintf(stderr, "FAIL: baking the index: %s\n", error.c_str());
    ++g_failures;
  }
  return bytes;
}

// Writes the world into an archive at `archive` and mounts it at "world://".
void MountWorld(const fs::path& archive, Vfs* vfs, bool skip_one_payload = false,
                u64 payload_bake_id = kBakeId) {
  PackWriter pack;
  pack.Add("city/city.rxworld", BakeIndex());
  for (u32 z = 0; z < 2; ++z) {
    for (u32 x = 0; x < 2; ++x) {
      const u64 id = CellId(x, z);
      if (skip_one_payload && id == 3) continue;
      CellPayloadWriter writer(id, Domain::kRepresentation, Tier::kFull);
      writer.set_bake_id(payload_bake_id);
      const u32 prototype = writer.AddPrototype("prop/rock");
      writer.AddInstance(id * 100, prototype, {}, {0, 0, 0, 1}, 1.0f);
      base::Vector<u8> bytes;
      std::string error;
      if (!writer.Encode(&bytes, &error)) {
        std::fprintf(stderr, "FAIL: baking a payload: %s\n", error.c_str());
        ++g_failures;
      }
      pack.Add(CellPayloadPath("city", id, Domain::kRepresentation, Tier::kFull), std::move(bytes));
    }
  }
  CHECK(pack.WriteTo(archive.string()));
  auto provider = rx::asset::MakePackFileProvider(archive.string());
  CHECK(provider != nullptr);
  if (provider) vfs->Mount("world", std::move(provider));
}

void TestLoadFromArchive(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "city.rxp", &vfs);

  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));
  CHECK(error.empty());
  CHECK(map.loaded());
  CHECK(map.index().cells.size() == 4);
  CHECK(map.index().bake_id == kBakeId);
  // The payload prefix follows the index, so a world can sit anywhere.
  CHECK(map.payload_prefix() == "world://city");

  WorldCellPayload payload;
  CHECK(map.ReadPayload(vfs, 2, Domain::kRepresentation, Tier::kFull, &payload, &error));
  CHECK(error.empty());
  CHECK(payload.instances.size() == 1);
  CHECK(payload.instances.size() == 1 && payload.instances[0].stable_id == 200);

  // A domain the cook produced nothing for, and a tier it did not bake.
  CHECK(!map.ReadPayload(vfs, 1, Domain::kCollision, Tier::kStandard, &payload, &error));
  CHECK(!error.empty());
  CHECK(!map.ReadPayload(vfs, 0, Domain::kRepresentation, Tier::kProxy, &payload, &error));

  CHECK(map.HasDomain(*map.index().FindCell(0), Domain::kCollision));
  CHECK(!map.HasDomain(*map.index().FindCell(1), Domain::kCollision));
  CHECK(map.HasDomain(*map.index().FindCell(3), Domain::kGameplay));
}

void TestMissingAndStaleArchives(const fs::path& directory) {
  fs::create_directories(directory);
  {
    Vfs vfs;
    WorldMap map;
    std::string error;
    CHECK(!map.Load(vfs, "world://city/city.rxworld", &error));
    CHECK(!error.empty());
    CHECK(!map.loaded());
  }
  {
    // The index lists cell 3's payload; the archive does not carry it. The read
    // must name the path rather than quietly produce an empty cell.
    Vfs vfs;
    MountWorld(directory / "holes.rxp", &vfs, /*skip_one_payload=*/true);
    WorldMap map;
    std::string error;
    CHECK(map.Load(vfs, "world://city/city.rxworld", &error));
    WorldCellPayload payload;
    CHECK(!map.ReadPayload(vfs, 3, Domain::kRepresentation, Tier::kFull, &payload, &error));
    CHECK(error.find("0000000000000003") != std::string::npos);
  }
  {
    // Payloads from a different cook than the index. This is the failure the
    // bake id exists for: every byte decodes, and every entity would be wrong.
    Vfs vfs;
    MountWorld(directory / "stale.rxp", &vfs, /*skip_one_payload=*/false,
               /*payload_bake_id=*/kBakeId + 1);
    WorldMap map;
    std::string error;
    CHECK(map.Load(vfs, "world://city/city.rxworld", &error));
    WorldCellPayload payload;
    CHECK(!map.ReadPayload(vfs, 0, Domain::kRepresentation, Tier::kFull, &payload, &error));
    CHECK(error.find("baked by") != std::string::npos);
  }
}

void TestPerDomainBubbles(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "bubbles.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));

  WorldStreamPolicy policy;
  policy[Domain::kGameplay].load_distance = 8;
  policy[Domain::kGameplay].retain_distance = 16;
  policy[Domain::kGameplay].full_tier_distance = 4;
  policy[Domain::kRepresentation].load_distance = 200;
  policy[Domain::kRepresentation].retain_distance = 240;
  policy[Domain::kRepresentation].full_tier_distance = 200;

  rx::scene::WorldStreamObservation observer;
  observer.position = {8, 0, 8};  // well inside cell 0
  observer.axes = rx::scene::kWorldStreamXZ;

  rx::scene::WorldStreamObservation gameplay;
  CHECK(MakeDomainObservation(observer, Domain::kGameplay, policy, &gameplay));
  CHECK(gameplay.load_distance == 8);
  CHECK(gameplay.channels == (1u << static_cast<u32>(Domain::kGameplay)));

  base::Vector<rx::scene::WorldStreamRegion> regions;
  map.GatherRegions(rx::scene::BuildWorldStreamQuery(gameplay), Domain::kGameplay, &regions);
  // The tight gameplay bubble reaches only its own cell.
  CHECK(regions.size() == 1);
  CHECK(regions.size() == 1 && regions[0].id == 0);

  regions.clear();
  rx::scene::WorldStreamObservation representation;
  CHECK(MakeDomainObservation(observer, Domain::kRepresentation, policy, &representation));
  map.GatherRegions(rx::scene::BuildWorldStreamQuery(representation), Domain::kRepresentation,
                    &regions);
  // The wide one reaches all four. Same observer, same frame, different bubble.
  CHECK(regions.size() == 4);

  // A disabled domain produces no observation at all, so nothing is gathered.
  rx::scene::WorldStreamObservation audio;
  CHECK(!MakeDomainObservation(observer, Domain::kAudio, policy, &audio));

  // An observer that has masked a domain off is not served it either.
  rx::scene::WorldStreamObservation masked = observer;
  masked.channels = 1u << static_cast<u32>(Domain::kRepresentation);
  CHECK(!MakeDomainObservation(masked, Domain::kGameplay, policy, &gameplay));

  // Only cell 0 has collision, so a query covering the whole world still finds
  // exactly one region for it.
  regions.clear();
  policy[Domain::kCollision].load_distance = 500;
  policy[Domain::kCollision].retain_distance = 500;
  rx::scene::WorldStreamObservation collision;
  CHECK(MakeDomainObservation(observer, Domain::kCollision, policy, &collision));
  map.GatherRegions(rx::scene::BuildWorldStreamQuery(collision), Domain::kCollision, &regions);
  CHECK(regions.size() == 1);
  CHECK(regions.size() == 1 && regions[0].id == 0);
}

void TestTargetTier(const fs::path& directory) {
  fs::create_directories(directory);
  Vfs vfs;
  MountWorld(directory / "tiers.rxp", &vfs);
  WorldMap map;
  std::string error;
  CHECK(map.Load(vfs, "world://city/city.rxworld", &error));
  const WorldCellRecord* cell = map.index().FindCell(0);
  CHECK(cell != nullptr);
  if (!cell) return;

  DomainStreamPolicy gameplay;
  gameplay.load_distance = 100;
  gameplay.full_tier_distance = 50;
  gameplay.near_tier = Tier::kFull;
  gameplay.far_tier = Tier::kProxy;

  // Near wants full; the cook only got as far as standard, so standard it is.
  CHECK(TargetTier(map.index(), *cell, Domain::kGameplay, gameplay, 10) == Tier::kStandard);
  CHECK(TargetTier(map.index(), *cell, Domain::kGameplay, gameplay, 80) == Tier::kProxy);

  // Representation was baked at full only. A far observer asking for proxy gets
  // the one tier that exists rather than a hole.
  DomainStreamPolicy representation = gameplay;
  CHECK(TargetTier(map.index(), *cell, Domain::kRepresentation, representation, 80) == Tier::kFull);

  // Nothing baked for this domain at all.
  CHECK(TargetTier(map.index(), *cell, Domain::kNavigation, gameplay, 10) == Tier::kAbsent);
}

}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / "rx_world_map_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  TestLoadFromArchive(tmp / "load");
  TestMissingAndStaleArchives(tmp / "stale");
  TestPerDomainBubbles(tmp / "bubbles");
  TestTargetTier(tmp / "tiers");

  fs::remove_all(tmp);
  if (g_failures) {
    std::fprintf(stderr, "world_map_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_map_test: ok");
  return 0;
}
