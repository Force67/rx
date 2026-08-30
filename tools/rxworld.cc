// rxworld — cooks an authored .rxscene into a baked, streamable world archive.
//
//   rxworld bake <scene.rxscene> <out.rxp> [--name city] [--cell-size 64]
//                [--instance <Component>] [--bake-id N] [--skip-unknown]
//   rxworld inspect <out.rxp> [--name city]
//
// bake reads the scene, sorts every entity into a grid cell by its world
// position, groups the entities of each cell by their component set, and writes
// one archetype-major payload per (cell, domain). The index goes in beside them
// under <name>/<name>.rxworld, so the whole world is one archive the engine
// mounts like any other content:
//
//   world/city/city.rxworld
//   world/city/0000000000000000.gameplay.standard.rxcell
//   world/city/0000000000000000.representation.full.rxcell
//
// Entities whose component set is exactly the one named by --instance (default:
// Transform + Renderable) are cooked as static instance pages instead of ECS
// rows: they get a stable world id and no entity until something promotes them.
// A Guid does not count towards that set - SaveScene puts one on everything it
// writes - and an instance carries no components at all, so a Guid on one is
// replaced by its stable id rather than baked.
//
// inspect prints the index of an already-baked archive, which is the cheapest
// way to see what a streaming decision will be working from.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <base/containers/vector.h>

#include "asset/pack.h"
#include "asset/vfs.h"
#include "world/world_bake.h"
#include "world/world_format.h"
#include "world/world_map.h"

namespace {

namespace asset = rx::asset;
namespace world = rx::world;

using rx::f32;
using rx::u32;
using rx::u64;

int Usage() {
  std::fprintf(stderr,
               "usage: rxworld bake <scene.rxscene> <out.rxp> [--name city] [--cell-size 64]\n"
               "                    [--instance <Component>] [--bake-id N] [--skip-unknown]\n"
               "       rxworld inspect <archive.rxp> [--name city]\n");
  return 2;
}

int Fail(const std::string& message) {
  std::fprintf(stderr, "rxworld: %s\n", message.c_str());
  return 1;
}

bool ParseU64(const char* text, u64* out) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (end == text || *end != '\0') return false;
  *out = value;
  return true;
}

int Bake(int argc, char** argv) {
  if (argc < 4) return Usage();
  const std::string scene_path = argv[2];
  const std::string archive_path = argv[3];

  world::WorldBakeOptions options;
  for (int i = 4; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (std::strcmp(argv[i], "--name") == 0 && has_value) {
      options.name = argv[++i];
    } else if (std::strcmp(argv[i], "--cell-size") == 0 && has_value) {
      options.cell_size = static_cast<f32>(std::atof(argv[++i]));
    } else if (std::strcmp(argv[i], "--instance") == 0 && has_value) {
      options.instance_components.push_back(argv[++i]);
    } else if (std::strcmp(argv[i], "--bake-id") == 0 && has_value) {
      if (!ParseU64(argv[++i], &options.bake_id)) return Fail("--bake-id must be a number");
    } else if (std::strcmp(argv[i], "--skip-unknown") == 0) {
      options.skip_unknown = true;
    } else {
      return Usage();
    }
  }

  world::WorldBakeResult result;
  std::string error;
  if (!world::BakeWorld(scene_path, options, archive_path, &result, &error)) return Fail(error);

  for (const std::string& name : result.dropped) {
    std::fprintf(stderr,
                 "rxworld: warning: '%s' holds an indirection and cannot be baked; every entity "
                 "carrying it loses it\n",
                 name.c_str());
  }
  std::printf("rxworld: %s -> %s\n", scene_path.c_str(), archive_path.c_str());
  std::printf("  %u cells, %u entities, %u static instances, bake %llu\n", result.cells,
              result.entities, result.instances,
              static_cast<unsigned long long>(result.bake_id));
  std::printf("  index at %s/%s.rxworld\n", options.name.c_str(), options.name.c_str());
  return 0;
}

int Inspect(int argc, char** argv) {
  if (argc < 3) return Usage();
  const std::string archive_path = argv[2];
  std::string name = "world";
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else {
      return Usage();
    }
  }

  asset::Vfs vfs;
  auto provider = asset::MakePackFileProvider(archive_path);
  if (!provider) return Fail(archive_path + ": not a readable .rxp");
  vfs.Mount("world", std::move(provider));

  world::WorldMap map;
  std::string error;
  if (!map.Load(vfs, "world://" + name + "/" + name + ".rxworld", &error)) return Fail(error);

  const world::WorldIndexData& index = map.index();
  std::printf("world %llu, bake %llu, %zu cells, cell size %.1f\n",
              static_cast<unsigned long long>(index.world_id),
              static_cast<unsigned long long>(index.bake_id), index.cells.size(),
              static_cast<double>(index.cell_size));
  for (const world::WorldCellRecord& cell : index.cells) {
    std::printf("  cell %016llx  [%.1f %.1f %.1f]..[%.1f %.1f %.1f]  ids %llu+%u\n",
                static_cast<unsigned long long>(cell.id), static_cast<double>(cell.minimum.x),
                static_cast<double>(cell.minimum.y), static_cast<double>(cell.minimum.z),
                static_cast<double>(cell.maximum.x), static_cast<double>(cell.maximum.y),
                static_cast<double>(cell.maximum.z),
                static_cast<unsigned long long>(cell.stable_id_first), cell.stable_id_count);
    for (u32 i = 0; i < cell.payload_count; ++i) {
      const world::WorldPayloadRecord& payload = index.payloads[cell.payload_first + i];
      std::printf("    %-15s %-9s %6u rows  %8llu bytes resident\n",
                  world::DomainName(payload.domain), world::TierName(payload.tier),
                  payload.row_count,
                  static_cast<unsigned long long>(payload.resident_bytes));
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  if (std::strcmp(argv[1], "bake") == 0) return Bake(argc, argv);
  if (std::strcmp(argv[1], "inspect") == 0) return Inspect(argc, argv);
  return Usage();
}
