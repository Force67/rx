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
//
// inspect prints the index of an already-baked archive, which is the cheapest
// way to see what a streaming decision will be working from.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "asset/pack.h"
#include "asset/vfs.h"
#include "ecs/world.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "scene/components.h"
#include "world/world_format.h"
#include "world/world_map.h"
#include "world/world_stream.h"

namespace {

namespace asset = rx::asset;
namespace ecs = rx::ecs;
namespace edit = rx::edit;
namespace scene = rx::scene;
namespace world = rx::world;

using rx::f32;
using rx::i64;
using rx::u32;
using rx::u64;
using rx::u8;

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

// One entity as the cook sees it: where it goes, and what it is made of.
struct Authored {
  ecs::Entity entity;
  u64 cell = 0;
  base::Vector<const edit::ComponentDesc*> components;  // sorted by name
  bool instance = false;
};

// The grid. Cells are ids on a 2D lattice over XZ (vertical extent is the
// cell's own bounds, not a third axis of the grid): a world tall enough to need
// stacked cells needs zones and portals, which the format carries but the
// baker does not yet author.
u64 CellIdFor(i64 x, i64 z) {
  // Interleave sign into the low bit so negative coordinates stay in a compact
  // unsigned range and the id ordering is stable.
  const u64 ux = x >= 0 ? static_cast<u64>(x) * 2 : static_cast<u64>(-x) * 2 - 1;
  const u64 uz = z >= 0 ? static_cast<u64>(z) * 2 : static_cast<u64>(-z) * 2 - 1;
  return (uz << 32) | (ux & 0xffffffffull);
}

i64 FloorDiv(f32 value, f32 size) {
  return static_cast<i64>(std::floor(static_cast<double>(value) / size));
}

bool ParseU64(const char* text, u64* out) {
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  if (end == text || *end != '\0') return false;
  *out = value;
  return true;
}

struct BakeOptions {
  std::string name = "world";
  f32 cell_size = 64.0f;
  u64 bake_id = 0;
  bool skip_unknown = false;
  base::Vector<std::string> instance_components;
};

u64 HashBytes(u64 hash, const void* data, size_t size) {
  const u8* bytes = static_cast<const u8*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// A content hash rather than a timestamp: baking the same scene with the same
// settings twice produces the same id, so a no-op rebuild does not invalidate
// every overlay keyed to it, while any change to the input or to how it is cut
// up produces a different one that an older index refuses to read.
u64 HashCook(const std::string& scene_path, const BakeOptions& options) {
  std::FILE* file = std::fopen(scene_path.c_str(), "rb");
  if (!file) return 0;
  u64 hash = 0xcbf29ce484222325ull;
  u8 buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    hash = HashBytes(hash, buffer, read);
  }
  std::fclose(file);
  hash = HashBytes(hash, options.name.data(), options.name.size());
  hash = HashBytes(hash, &options.cell_size, sizeof(options.cell_size));
  for (const std::string& component : options.instance_components) {
    hash = HashBytes(hash, component.data(), component.size());
  }
  return hash == 0 ? 1 : hash;  // 0 means "unreadable"
}

// Whether a component can be cooked at all. Anything holding an indirection is
// restored by copying bytes at load, so it cannot be; the caller is told which
// ones were dropped rather than left to notice at runtime.
bool Bakeable(const edit::ComponentDesc& desc) {
  return ecs::GetComponentInfo(desc.id).trivially_copyable;
}

bool SameComponents(const Authored& a, const Authored& b) {
  if (a.instance != b.instance || a.components.size() != b.components.size()) return false;
  for (size_t i = 0; i < a.components.size(); ++i) {
    if (a.components[i] != b.components[i]) return false;
  }
  return true;
}

int Bake(int argc, char** argv) {
  if (argc < 4) return Usage();
  const std::string scene_path = argv[2];
  const std::string archive_path = argv[3];

  BakeOptions options;
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
  if (!(options.cell_size > 0)) return Fail("--cell-size must be positive");
  if (options.instance_components.empty()) {
    options.instance_components.push_back("Transform");
    options.instance_components.push_back("Renderable");
  }
  if (options.bake_id == 0) options.bake_id = HashCook(scene_path, options);
  if (options.bake_id == 0) return Fail(scene_path + ": cannot be read");

  asset::Vfs vfs;
  asset::AssetDatabase database(vfs);
  ecs::World source;
  std::string error;
  // Strict by default: a component this build does not register is dropped
  // silently otherwise, and the cell that needed it bakes without the thing it
  // was authored to place. A cook that links a game's own component
  // registrations sees them all; one that does not should say so and stop.
  if (!edit::LoadScene(source, database, scene_path, &error, !options.skip_unknown)) {
    if (!options.skip_unknown) {
      error +=
          "\n  (rxworld reflects only the components this build registers; pass --skip-unknown to "
          "bake without them, or cook from a build that registers the game's own)";
    }
    return Fail(error);
  }

  // Sort every entity into a cell, and record what it is made of.
  base::Vector<Authored> authored;
  base::Vector<std::string> dropped;
  u32 parented = 0;
  source.Each<scene::Transform>([&](ecs::Entity entity, scene::Transform& transform) {
    if (source.Has<scene::Parent>(entity)) {
      ++parented;
      return;
    }
    Authored record;
    record.entity = entity;
    record.cell = CellIdFor(FloorDiv(transform.position[0], options.cell_size),
                            FloorDiv(transform.position[2], options.cell_size));
    for (const edit::ComponentDesc* desc : edit::ComponentsOn(source, entity)) {
      if (!Bakeable(*desc)) {
        bool seen = false;
        for (const std::string& name : dropped) seen |= name == desc->name;
        if (!seen) dropped.push_back(desc->name);
        continue;
      }
      record.components.push_back(desc);
    }
    std::sort(record.components.begin(), record.components.end(),
              [](const edit::ComponentDesc* a, const edit::ComponentDesc* b) {
                return std::strcmp(a->name, b->name) < 0;
              });
    record.instance = record.components.size() == options.instance_components.size();
    for (size_t i = 0; record.instance && i < record.components.size(); ++i) {
      bool named = false;
      for (const std::string& want : options.instance_components) {
        named |= want == record.components[i]->name;
      }
      record.instance = named;
    }
    authored.push_back(std::move(record));
  });

  // A Parent link is an ecs::Entity handle, and a handle cannot survive a bake,
  // let alone a streaming boundary. Refusing is the honest answer: silently
  // dropping the link would move the child into world space.
  if (parented != 0) {
    return Fail(std::to_string(parented) +
                " entities have a Parent; flatten the hierarchy before baking (a parent link is "
                "an ecs handle, which no baked cell can carry)");
  }
  if (authored.empty()) return Fail(scene_path + ": no entities with a Transform to bake");

  for (const std::string& name : dropped) {
    std::fprintf(stderr, "rxworld: warning: dropping '%s', which holds an indirection\n",
                 name.c_str());
  }

  // Group by cell, then by component set. Sorting once gives both.
  std::sort(authored.begin(), authored.end(), [](const Authored& a, const Authored& b) {
    if (a.cell != b.cell) return a.cell < b.cell;
    if (a.instance != b.instance) return a.instance < b.instance;
    if (a.components.size() != b.components.size()) {
      return a.components.size() < b.components.size();
    }
    for (size_t i = 0; i < a.components.size(); ++i) {
      const int order = std::strcmp(a.components[i]->name, b.components[i]->name);
      if (order != 0) return order < 0;
    }
    return false;
  });

  world::WorldIndexWriter index;
  index.set_world_id(asset::MakeAssetId(options.name).hash);
  index.set_bake_id(options.bake_id);
  index.set_grid(options.cell_size, {0, 0, 0});
  asset::PackWriter pack;
  const std::string prefix = options.name;

  u64 next_stable_id = 1;  // 0 is reserved for "no id"
  u32 cell_count = 0;
  u32 entity_count = 0;
  u32 instance_count = 0;

  for (size_t cell_begin = 0; cell_begin < authored.size();) {
    const u64 cell = authored[cell_begin].cell;
    size_t cell_end = cell_begin;
    while (cell_end < authored.size() && authored[cell_end].cell == cell) ++cell_end;

    const u64 stable_id_first = next_stable_id;
    const u32 stable_id_count = static_cast<u32>(cell_end - cell_begin);
    // Bounds are the cell's lattice square on XZ; vertically they follow the
    // contents, padded by half a cell above and below. The baker knows where
    // entities are, not how large they are, so the padding stands in for the
    // extent it cannot see - and it keeps a cell whose contents all sit at one
    // height from having zero thickness, which an observer streaming on all
    // three axes would only ever match by landing exactly on that plane.
    f32 low_y = 0;
    f32 high_y = 0;
    bool first_y = true;
    for (size_t i = cell_begin; i < cell_end; ++i) {
      const scene::Transform* transform = source.Get<scene::Transform>(authored[i].entity);
      if (!transform) continue;
      low_y = first_y ? transform->position[1] : std::min(low_y, transform->position[1]);
      high_y = first_y ? transform->position[1] : std::max(high_y, transform->position[1]);
      first_y = false;
    }
    const scene::Transform* any = source.Get<scene::Transform>(authored[cell_begin].entity);
    const f32 base_x = std::floor(any->position[0] / options.cell_size) * options.cell_size;
    const f32 base_z = std::floor(any->position[2] / options.cell_size) * options.cell_size;
    const f32 pad = options.cell_size * 0.5f;
    index.AddCell(cell, {base_x, low_y - pad, base_z},
                  {base_x + options.cell_size, high_y + pad, base_z + options.cell_size}, 0,
                  stable_id_first, stable_id_count);
    ++cell_count;

    // Instance page: everything in this cell whose component set is the
    // instance set, packed with no ECS identity at all.
    world::CellPayloadWriter instances(cell, world::Domain::kRepresentation, world::Tier::kFull);
    instances.set_bake_id(options.bake_id);
    world::CellPayloadWriter entities(cell, world::Domain::kGameplay, world::Tier::kStandard);
    entities.set_bake_id(options.bake_id);
    u32 cell_instances = 0;
    u32 cell_entities = 0;

    for (size_t group_begin = cell_begin; group_begin < cell_end;) {
      size_t group_end = group_begin;
      while (group_end < cell_end && SameComponents(authored[group_begin], authored[group_end])) {
        ++group_end;
      }
      const u32 rows = static_cast<u32>(group_end - group_begin);

      if (authored[group_begin].instance) {
        for (size_t i = group_begin; i < group_end; ++i) {
          const scene::Transform* transform = source.Get<scene::Transform>(authored[i].entity);
          const scene::Renderable* renderable = source.Get<scene::Renderable>(authored[i].entity);
          const std::string prototype =
              renderable ? asset::LookupAssetPath(renderable->mesh).value_or(
                               std::to_string(renderable->mesh.hash))
                         : std::string("prototype/unnamed");
          instances.AddInstance(
              next_stable_id + (i - cell_begin), instances.AddPrototype(prototype),
              {transform->position[0], transform->position[1], transform->position[2]},
              {transform->rotation[0], transform->rotation[1], transform->rotation[2],
               transform->rotation[3]},
              transform->scale);
          ++cell_instances;
        }
        group_begin = group_end;
        continue;
      }

      const u32 archetype = entities.BeginArchetype(rows);
      base::Vector<u64> ids;
      ids.reserve(rows);
      for (size_t i = group_begin; i < group_end; ++i) {
        ids.push_back(next_stable_id + (i - cell_begin));
      }
      entities.SetStableIds(archetype, std::span<const u64>(ids.data(), ids.size()));

      for (const edit::ComponentDesc* desc : authored[group_begin].components) {
        u32 stride = 0;
        u64 layout = 0;
        if (!world::RuntimeComponentLayout(desc->name, &stride, &layout)) {
          return Fail(std::string("component '") + desc->name + "' has no reflected layout");
        }
        base::Vector<u8> column;
        column.reserve(static_cast<size_t>(stride) * rows);
        for (size_t i = group_begin; i < group_end; ++i) {
          const void* value = source.GetRaw(authored[i].entity, desc->id);
          if (!value) return Fail(std::string("component '") + desc->name + "' vanished mid-bake");
          const u8* bytes = static_cast<const u8*>(value);
          column.insert(column.end(), bytes, bytes + stride);
        }
        entities.AddColumn(archetype, desc->name, stride, layout,
                           std::span<const u8>(column.data(), column.size()));
      }
      cell_entities += rows;
      group_begin = group_end;
    }

    if (cell_entities != 0) {
      base::Vector<u8> bytes;
      if (!entities.Encode(&bytes, &error)) return Fail(error);
      index.AddPayload(cell, world::Domain::kGameplay, world::Tier::kStandard, bytes.size(),
                       cell_entities);
      pack.Add(world::CellPayloadPath(prefix, cell, world::Domain::kGameplay,
                                      world::Tier::kStandard),
               std::move(bytes));
      entity_count += cell_entities;
    }
    if (cell_instances != 0) {
      base::Vector<u8> bytes;
      if (!instances.Encode(&bytes, &error)) return Fail(error);
      index.AddPayload(cell, world::Domain::kRepresentation, world::Tier::kFull, bytes.size(),
                       cell_instances);
      pack.Add(world::CellPayloadPath(prefix, cell, world::Domain::kRepresentation,
                                      world::Tier::kFull),
               std::move(bytes));
      instance_count += cell_instances;
    }

    next_stable_id += stable_id_count;
    cell_begin = cell_end;
  }

  base::Vector<u8> index_bytes;
  if (!index.Encode(&index_bytes, &error)) return Fail(error);
  pack.Add(prefix + "/" + options.name + ".rxworld", std::move(index_bytes));

  if (!pack.WriteTo(archive_path)) return Fail(archive_path + ": cannot be written");

  std::printf("rxworld: %s -> %s\n", scene_path.c_str(), archive_path.c_str());
  std::printf("  %u cells, %u entities, %u static instances, bake %llu\n", cell_count,
              entity_count, instance_count,
              static_cast<unsigned long long>(options.bake_id));
  std::printf("  index at %s/%s.rxworld\n", prefix.c_str(), options.name.c_str());
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
