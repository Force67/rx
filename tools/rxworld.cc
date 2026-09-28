// rxworld cooks an authored .rxscene into a baked, streamable world archive.
//
//   rxworld bake <scene.rxscene> <out.rxp> [--name city] [--cell-size 64]
//                [--instance <Component>] [--bake-id N] [--skip-unknown]
//   rxworld inspect <out.rxp> [--name city]
//
// --name defaults to the archive's filename stem for both. bake sorts every
// entity into a grid cell by world position, groups each cell's entities by
// component set, and writes one archetype-major payload per (cell, domain); the
// index goes in beside them as <name>/<name>.rxworld, so the world is one
// archive the engine mounts like any other content.
//
// Entities whose component set is exactly --instance (default Transform +
// Renderable) are cooked as static instance pages instead of ECS rows: stable
// world id, no entity until something promotes them. A Guid does not count
// towards that set (SaveScene puts one on everything) and is replaced by the
// stable id. inspect prints the index: the cheapest view of what a streaming
// decision works from.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/containers/vector.h>

#include "base/memory/move.h"
#include "base/strings/xstring.h"
#include "rxe/asset/pack.h"
#include "rxe/asset/vfs.h"
#include "rxe/world/world_bake.h"
#include "rxe/world/world_format.h"
#include "rxe/world/world_map.h"

namespace {

namespace asset = rx::asset;
namespace world = rx::world;

using rx::f32;
using rx::u32;
using rx::u64;

int Usage() {
  ::fprintf(stderr,
               "usage: rxworld bake <scene.rxscene> <out.rxp> [--name city] [--cell-size 64]\n"
               "                    [--instance <Component>] [--bake-id N] [--skip-unknown]\n"
               "       rxworld inspect <archive.rxp> [--name city]\n");
  return 2;
}

int Fail(const base::String& message) {
  ::fprintf(stderr, "rxworld: %s\n", message.c_str());
  return 1;
}

bool ParseU64(const char* text, u64* out) {
  char* end = nullptr;
  const unsigned long long value = ::strtoull(text, &end, 0);
  if (end == text || *end != '\0') return false;
  *out = value;
  return true;
}

int Bake(int argc, char** argv) {
  if (argc < 4) return Usage();
  const base::String scene_path = argv[2];
  const base::String archive_path = argv[3];

  world::WorldBakeOptions options;
  for (int i = 4; i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (::strcmp(argv[i], "--name") == 0 && has_value) {
      options.name = argv[++i];
    } else if (::strcmp(argv[i], "--cell-size") == 0 && has_value) {
      options.cell_size = static_cast<f32>(::atof(argv[++i]));
    } else if (::strcmp(argv[i], "--instance") == 0 && has_value) {
      options.instance_components.push_back(argv[++i]);
    } else if (::strcmp(argv[i], "--bake-id") == 0 && has_value) {
      if (!ParseU64(argv[++i], &options.bake_id)) return Fail("--bake-id must be a number");
    } else if (::strcmp(argv[i], "--skip-unknown") == 0) {
      options.skip_unknown = true;
    } else {
      return Usage();
    }
  }

  world::WorldBakeResult result;
  base::String error;
  if (!world::BakeWorld(scene_path, options, archive_path, &result, &error)) return Fail(error);

  for (const base::String& name : result.dropped) {
    ::fprintf(stderr,
                 "rxworld: warning: '%s' holds an indirection and cannot be baked; every entity "
                 "carrying it loses it\n",
                 name.c_str());
  }
  ::printf("rxworld: %s -> %s\n", scene_path.c_str(), archive_path.c_str());
  ::printf("  %u cells, %u entities, %u static instances, bake %llu\n", result.cells,
              result.entities, result.instances,
              static_cast<unsigned long long>(result.bake_id));
  ::printf("  index at %s/%s.rxworld\n", result.name.c_str(), result.name.c_str());
  return 0;
}

int Inspect(int argc, char** argv) {
  if (argc < 3) return Usage();
  const base::String archive_path = argv[2];
  // Same default the cook uses, so inspecting an archive baked with no --name
  // needs no --name either.
  base::String name = world::WorldNameForArchive(archive_path);
  for (int i = 3; i < argc; ++i) {
    if (::strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
      name = argv[++i];
    } else {
      return Usage();
    }
  }

  asset::Vfs vfs;
  auto provider = asset::MakePackFileProvider(archive_path);
  if (!provider) return Fail(archive_path + ": not a readable .rxp");
  vfs.Mount("world", base::move(provider));

  world::WorldMap map;
  base::String error;
  if (!map.Load(vfs, "world://" + name + "/" + name + ".rxworld", &error)) return Fail(error);

  const world::WorldIndexData& index = map.index();
  ::printf("world %llu, bake %llu, %llu cells, cell size %.1f\n",
              static_cast<unsigned long long>(index.world_id),
              static_cast<unsigned long long>(index.bake_id),
              static_cast<unsigned long long>(index.cells.size()),
              static_cast<double>(index.cell_size));
  for (const world::WorldCellRecord& cell : index.cells) {
    ::printf("  cell %016llx  [%.1f %.1f %.1f]..[%.1f %.1f %.1f]  ids %llu+%u\n",
                static_cast<unsigned long long>(cell.id), static_cast<double>(cell.minimum.x),
                static_cast<double>(cell.minimum.y), static_cast<double>(cell.minimum.z),
                static_cast<double>(cell.maximum.x), static_cast<double>(cell.maximum.y),
                static_cast<double>(cell.maximum.z),
                static_cast<unsigned long long>(cell.stable_id_first), cell.stable_id_count);
    for (u32 i = 0; i < cell.payload_count; ++i) {
      const world::WorldPayloadRecord& payload = index.payloads[cell.payload_first + i];
      ::printf("    %-15s %-9s %6u rows  %8llu bytes resident\n",
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
  if (::strcmp(argv[1], "bake") == 0) return Bake(argc, argv);
  if (::strcmp(argv[1], "inspect") == 0) return Inspect(argc, argv);
  return Usage();
}
