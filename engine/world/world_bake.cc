#include "world/world_bake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "asset/asset_database.h"
#include "asset/asset_id.h"
#include "asset/pack.h"
#include "asset/vfs.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "scene/components.h"
#include "world/world_format.h"
#include "world/world_stream.h"

namespace rx::world {
namespace {

// The lattice is packed 32 bits per axis, and the sign fold below doubles the
// magnitude, so a coordinate this far out would alias onto another cell rather
// than fail. Far beyond any real world: at 64 m cells this is 2^29 * 64 m.
constexpr i64 kMaximumCellCoord = i64{1} << 29;

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

// One entity as the cook sees it: where it goes, and what it is made of.
struct Authored {
  ecs::Entity entity;
  u64 cell = 0;
  i64 cell_x = 0;
  i64 cell_z = 0;
  base::Vector<const edit::ComponentDesc*> components;  // bakeable, sorted by name
  bool instance = false;
};

// Identity a baked world already has a better answer for. SaveScene puts a Guid
// on every entity it writes, so without this nothing an editor produced would
// ever classify as static decoration; the stable id replaces it, and carrying
// both would mean the instance page's rows each needed an ECS row to hold one.
bool IdentityOnly(const edit::ComponentDesc& desc) {
  return std::strcmp(desc.name, "Guid") == 0;
}

// The grid. Cells are ids on a 2D lattice over XZ (vertical extent is the
// cell's own bounds, not a third axis of the grid): a world tall enough to need
// stacked cells needs zones and portals, which the format carries but the cook
// does not yet author.
u64 CellIdFor(i64 x, i64 z) {
  // Interleave sign into the low bit so negative coordinates stay in a compact
  // unsigned range and the id ordering is stable.
  const u64 ux = x >= 0 ? static_cast<u64>(x) * 2 : static_cast<u64>(-x) * 2 - 1;
  const u64 uz = z >= 0 ? static_cast<u64>(z) * 2 : static_cast<u64>(-z) * 2 - 1;
  return (uz << 32) | (ux & 0xffffffffull);
}

// The cell a coordinate falls in, and the lattice origin of that cell. Both go
// through the same division, so the bounds a cell is given always contain the
// entities binned into it - computing the two separately in different
// precisions puts an AABB one square away from its own contents.
i64 CellCoord(f32 value, f32 size) {
  return static_cast<i64>(std::floor(static_cast<double>(value) / size));
}

f32 CellOrigin(i64 coord, f32 size) { return static_cast<f32>(static_cast<double>(coord) * size); }

// Finite and inside the lattice. The magnitude check is not paranoia: without
// it the cast in CellCoord is undefined for a large enough quotient, and a
// merely large one folds two distant entities into one cell whose bounds
// contain neither of them - a cook that succeeds having written a world that
// is wrong rather than one that fails to load.
bool InLattice(const scene::Transform& transform, f32 cell_size) {
  for (u32 axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(transform.position[axis])) return false;
  }
  for (u32 axis = 0; axis < 3; axis += 2) {
    const double quotient = static_cast<double>(transform.position[axis]) / cell_size;
    if (!(std::abs(quotient) < static_cast<double>(kMaximumCellCoord))) return false;
  }
  return true;
}

// Whether a component can be cooked at all: it has to be restorable by copying
// bytes, and its bytes have to still mean something afterwards. An entity
// reference fails the second test even though it passes the first - a handle is
// reused with a new generation once its slot is freed - so it is refused here
// as well as at load.
bool Bakeable(const edit::ComponentDesc& desc) {
  if (!ecs::GetComponentInfo(desc.id).trivially_copyable) return false;
  for (u32 i = 0; i < desc.prop_count; ++i) {
    if (desc.props[i].type == edit::PropType::kEntity) return false;
  }
  return true;
}

u64 HashBytes(u64 hash, const void* data, size_t size) {
  const u8* bytes = static_cast<const u8*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// Length first, then the bytes. Concatenating variable-length strings without
// it makes the hash ambiguous: {"A", "BC"} and {"AB", "C"} feed it the same
// sequence, so two cooks that cut the world differently would claim the same
// bake id and each accept the other's saves.
u64 HashString(u64 hash, std::string_view value) {
  const u64 size = value.size();
  hash = HashBytes(hash, &size, sizeof(size));
  return HashBytes(hash, value.data(), value.size());
}

// A content hash rather than a timestamp: baking the same scene with the same
// settings twice produces the same id, so a no-op rebuild does not invalidate
// every overlay keyed to it, while any change to the input or to how it is cut
// up produces a different one that an older index refuses to read.
//
// `schema` is the sorted name and reflected layout of every component this cook
// actually writes. Only those: folding in the whole registry would move every
// world's bake id whenever an unrelated component was added anywhere in the
// engine, invalidating the saves the id exists to protect.
u64 HashCook(const std::string& scene_path, const WorldBakeOptions& options,
             const base::Vector<std::string>& schema) {
  std::FILE* file = std::fopen(scene_path.c_str(), "rb");
  if (!file) return 0;
  u64 hash = 0xcbf29ce484222325ull;
  u8 buffer[4096];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    hash = HashBytes(hash, buffer, read);
  }
  std::fclose(file);
  hash = HashString(hash, options.name);
  hash = HashBytes(hash, &options.cell_size, sizeof(options.cell_size));
  hash = HashBytes(hash, &options.skip_unknown, sizeof(options.skip_unknown));
  const u64 instance_count = options.instance_components.size();
  hash = HashBytes(hash, &instance_count, sizeof(instance_count));
  for (const std::string& component : options.instance_components) {
    hash = HashString(hash, component);
  }
  const u64 schema_count = schema.size();
  hash = HashBytes(hash, &schema_count, sizeof(schema_count));
  for (const std::string& entry : schema) hash = HashString(hash, entry);
  return hash == 0 ? 1 : hash;  // 0 means "unreadable"
}

bool SameComponents(const Authored& a, const Authored& b) {
  if (a.instance != b.instance || a.components.size() != b.components.size()) return false;
  for (size_t i = 0; i < a.components.size(); ++i) {
    if (a.components[i] != b.components[i]) return false;
  }
  return true;
}

// The instance set the options ask for, with the default filled in.
base::Vector<std::string> InstanceComponents(const WorldBakeOptions& options) {
  if (!options.instance_components.empty()) return options.instance_components;
  base::Vector<std::string> defaults;
  defaults.push_back("Transform");
  defaults.push_back("Renderable");
  return defaults;
}

}  // namespace

const char* BakeRoleName(BakeRole role) {
  switch (role) {
    case BakeRole::kEntity: return "gameplay entity";
    case BakeRole::kInstance: return "instance page row";
    case BakeRole::kRefused: return "refused";
  }
  return "unknown";
}

BakeVerdict ClassifyForBake(ecs::World& world, ecs::Entity entity,
                            const WorldBakeOptions& options) {
  BakeVerdict verdict;
  const base::Vector<std::string> instance_set = InstanceComponents(options);

  const scene::Transform* transform = world.Get<scene::Transform>(entity);
  if (!transform) {
    verdict.role = BakeRole::kRefused;
    verdict.refusal = "no Transform, so the cook has nowhere to put it";
    return verdict;
  }
  if (world.Has<scene::Parent>(entity)) {
    verdict.role = BakeRole::kRefused;
    verdict.refusal = "has a Parent, which is an ecs handle no baked cell can carry";
    return verdict;
  }
  const f32 cell_size = options.cell_size > 0 && std::isfinite(options.cell_size)
                            ? options.cell_size
                            : 64.0f;
  if (!InLattice(*transform, cell_size)) {
    verdict.role = BakeRole::kRefused;
    verdict.refusal = "its position is not finite, or so far out that its cell would alias";
    return verdict;
  }

  const i64 cell_x = CellCoord(transform->position[0], cell_size);
  const i64 cell_z = CellCoord(transform->position[2], cell_size);
  verdict.has_cell = true;
  verdict.cell = CellIdFor(cell_x, cell_z);
  const f32 base_x = CellOrigin(cell_x, cell_size);
  const f32 base_z = CellOrigin(cell_z, cell_size);
  verdict.cell_minimum = {base_x, 0, base_z};
  verdict.cell_maximum = {base_x + cell_size, 0, base_z + cell_size};

  // Classify from what was authored, not from what survives the drop: an entity
  // that is a transform, a mesh and a Name is an entity that happens to lose
  // its Name, not a static instance.
  u32 authored_count = 0;
  bool only_instance_components = true;
  for (const edit::ComponentDesc* desc : edit::ComponentsOn(world, entity)) {
    if (!IdentityOnly(*desc)) {
      ++authored_count;
      bool named = false;
      for (const std::string& want : instance_set) named |= want == desc->name;
      only_instance_components &= named;
    }
    if (!Bakeable(*desc)) verdict.dropped.push_back(desc->name);
  }
  verdict.role = only_instance_components && authored_count == instance_set.size()
                     ? BakeRole::kInstance
                     : BakeRole::kEntity;
  return verdict;
}

bool BakeWorld(const std::string& scene_path, const WorldBakeOptions& input_options,
               const std::string& archive_path, WorldBakeResult* result, std::string* error) {
  if (!result) return false;
  *result = WorldBakeResult{};

  WorldBakeOptions options = input_options;
  if (!(options.cell_size > 0) || !std::isfinite(options.cell_size)) {
    SetError(error, "cell size must be a positive, finite number");
    return false;
  }
  options.instance_components = InstanceComponents(options);

  asset::Vfs vfs;
  asset::AssetDatabase database(vfs);
  // A fresh world, which is what makes stable-id assignment deterministic:
  // entity indices are the file's order, so two cooks of one file replay the
  // same creation sequence and hand out the same ids.
  ecs::World source;
  std::string load_error;
  // Strict by default: a component this build does not register is dropped
  // silently otherwise, and the cell that needed it bakes without the thing it
  // was authored to place.
  if (!edit::LoadScene(source, database, scene_path, &load_error, !options.skip_unknown)) {
    if (!options.skip_unknown) {
      load_error +=
          "\n  (this build reflects only the components it registers; allow unknown components to "
          "bake without them, or cook from a build that registers the game's own)";
    }
    SetError(error, load_error);
    return false;
  }

  base::Vector<Authored> authored;
  u32 parented = 0;
  u32 off_lattice = 0;
  source.Each<scene::Transform>([&](ecs::Entity entity, scene::Transform& transform) {
    if (source.Has<scene::Parent>(entity)) {
      ++parented;
      return;
    }
    if (!InLattice(transform, options.cell_size)) {
      ++off_lattice;
      return;
    }
    Authored record;
    record.entity = entity;
    record.cell_x = CellCoord(transform.position[0], options.cell_size);
    record.cell_z = CellCoord(transform.position[2], options.cell_size);
    record.cell = CellIdFor(record.cell_x, record.cell_z);

    u32 authored_count = 0;
    bool only_instance_components = true;
    for (const edit::ComponentDesc* desc : edit::ComponentsOn(source, entity)) {
      // Identity-only components do not decide what a thing is, but they are
      // still baked onto anything that stays an entity.
      if (!IdentityOnly(*desc)) {
        ++authored_count;
        bool named = false;
        for (const std::string& want : options.instance_components) named |= want == desc->name;
        only_instance_components &= named;
      }
      if (!Bakeable(*desc)) {
        bool seen = false;
        for (const std::string& name : result->dropped) seen |= name == desc->name;
        if (!seen) result->dropped.push_back(desc->name);
        continue;
      }
      record.components.push_back(desc);
    }
    std::sort(record.components.begin(), record.components.end(),
              [](const edit::ComponentDesc* a, const edit::ComponentDesc* b) {
                return std::strcmp(a->name, b->name) < 0;
              });
    record.instance =
        only_instance_components && authored_count == options.instance_components.size();
    authored.push_back(std::move(record));
  });

  // A Parent link is an ecs::Entity handle, and a handle cannot survive a bake,
  // let alone a streaming boundary. Refusing is the honest answer: silently
  // dropping the link would move the child into world space.
  if (parented != 0) {
    SetError(error, std::to_string(parented) +
                        " entities have a Parent; flatten the hierarchy before baking (a parent "
                        "link is an ecs handle, which no baked cell can carry)");
    return false;
  }
  if (off_lattice != 0) {
    SetError(error, std::to_string(off_lattice) +
                        " entities have a Transform.position that is not finite, or so far out "
                        "that its cell would alias onto another; either way the archive would "
                        "cook and be wrong");
    return false;
  }
  if (authored.empty()) {
    SetError(error, scene_path + ": no entities with a Transform to bake");
    return false;
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
    // A total order, so std::sort's instability cannot decide which entity gets
    // which stable id. The index is deterministic because the cook loaded this
    // file into a fresh world, so index order is file order; cooking a lived-in
    // world would order ties by that session's history instead, which is why
    // BakeWorld takes a path rather than an ecs::World.
    return a.entity.index < b.entity.index;
  });

  // The schema this cook actually writes, sorted, with each component's
  // reflected layout: a build that disagrees about any of it produces different
  // bytes for the same scene and so has to be a different bake.
  base::Vector<const edit::ComponentDesc*> written;
  for (const Authored& record : authored) {
    for (const edit::ComponentDesc* desc : record.components) {
      bool seen = false;
      for (const edit::ComponentDesc* entry : written) seen |= entry == desc;
      if (!seen) written.push_back(desc);
    }
  }
  std::sort(written.begin(), written.end(),
            [](const edit::ComponentDesc* a, const edit::ComponentDesc* b) {
              return std::strcmp(a->name, b->name) < 0;
            });
  base::Vector<std::string> schema;
  schema.reserve(written.size());
  for (const edit::ComponentDesc* desc : written) {
    u32 stride = 0;
    u64 layout = 0;
    if (!RuntimeComponentLayout(desc->name, &stride, &layout)) {
      SetError(error, std::string("component '") + desc->name + "' has no reflected layout");
      return false;
    }
    schema.push_back(std::string(desc->name) + " " + std::to_string(layout));
  }

  if (options.bake_id == 0) options.bake_id = HashCook(scene_path, options, schema);
  if (options.bake_id == 0) {
    SetError(error, scene_path + ": cannot be read");
    return false;
  }

  WorldIndexWriter index;
  index.set_world_id(asset::MakeAssetId(options.name).hash);
  index.set_bake_id(options.bake_id);
  index.set_grid(options.cell_size, {0, 0, 0});
  asset::PackWriter pack;
  const std::string prefix = options.name;

  u64 next_stable_id = 1;  // 0 is reserved for "no id"
  std::string encode_error;

  for (size_t cell_begin = 0; cell_begin < authored.size();) {
    const u64 cell = authored[cell_begin].cell;
    size_t cell_end = cell_begin;
    while (cell_end < authored.size() && authored[cell_end].cell == cell) ++cell_end;

    const u64 stable_id_first = next_stable_id;
    const u32 stable_id_count = static_cast<u32>(cell_end - cell_begin);
    // Bounds are the cell's lattice square on XZ; vertically they follow the
    // contents, padded by half a cell above and below. The cook knows where
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
    const f32 base_x = CellOrigin(authored[cell_begin].cell_x, options.cell_size);
    const f32 base_z = CellOrigin(authored[cell_begin].cell_z, options.cell_size);
    const f32 pad = options.cell_size * 0.5f;
    index.AddCell(cell, {base_x, low_y - pad, base_z},
                  {base_x + options.cell_size, high_y + pad, base_z + options.cell_size}, 0,
                  stable_id_first, stable_id_count);
    ++result->cells;

    CellPayloadWriter instances(cell, Domain::kRepresentation, Tier::kFull);
    instances.set_bake_id(options.bake_id);
    CellPayloadWriter entities(cell, Domain::kGameplay, Tier::kStandard);
    entities.set_bake_id(options.bake_id);
    u32 cell_instances = 0;
    u32 cell_entities = 0;
    u64 cell_entity_bytes = 0;

    for (size_t group_begin = cell_begin; group_begin < cell_end;) {
      size_t group_end = group_begin;
      while (group_end < cell_end && SameComponents(authored[group_begin], authored[group_end])) {
        ++group_end;
      }
      const u32 rows = static_cast<u32>(group_end - group_begin);

      if (authored[group_begin].instance) {
        for (size_t i = group_begin; i < group_end; ++i) {
          const scene::Transform* transform = source.Get<scene::Transform>(authored[i].entity);
          if (!transform) {
            SetError(error, "an entity lost its Transform mid-bake");
            return false;
          }
          const scene::Renderable* renderable = source.Get<scene::Renderable>(authored[i].entity);
          const std::string prototype =
              renderable ? asset::LookupAssetPath(renderable->mesh)
                               .value_or(std::to_string(renderable->mesh.hash))
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
        if (!RuntimeComponentLayout(desc->name, &stride, &layout)) {
          SetError(error, std::string("component '") + desc->name + "' has no reflected layout");
          return false;
        }
        base::Vector<u8> column;
        column.reserve(static_cast<size_t>(stride) * rows);
        for (size_t i = group_begin; i < group_end; ++i) {
          const void* value = source.GetRaw(authored[i].entity, desc->id);
          if (!value) {
            SetError(error, std::string("component '") + desc->name + "' vanished mid-bake");
            return false;
          }
          const u8* bytes = static_cast<const u8*>(value);
          column.insert(column.end(), bytes, bytes + stride);
        }
        entities.AddColumn(archetype, desc->name, stride, layout,
                           std::span<const u8>(column.data(), column.size()));
        cell_entity_bytes += column.size();
      }
      // What the rows cost once they are ECS rows, which is the number the
      // memory budget wants: the columns plus the CellResident the streamer
      // adds. The encoded payload is a different, smaller number.
      cell_entity_bytes += static_cast<u64>(rows) * sizeof(CellResident);
      cell_entities += rows;
      group_begin = group_end;
    }

    if (cell_entities != 0) {
      base::Vector<u8> bytes;
      if (!entities.Encode(&bytes, &encode_error)) {
        SetError(error, encode_error);
        return false;
      }
      index.AddPayload(cell, Domain::kGameplay, Tier::kStandard, cell_entity_bytes, cell_entities);
      pack.Add(CellPayloadPath(prefix, cell, Domain::kGameplay, Tier::kStandard), std::move(bytes));
      result->entities += cell_entities;
    }
    if (cell_instances != 0) {
      base::Vector<u8> bytes;
      if (!instances.Encode(&bytes, &encode_error)) {
        SetError(error, encode_error);
        return false;
      }
      index.AddPayload(cell, Domain::kRepresentation, Tier::kFull,
                       static_cast<u64>(cell_instances) * sizeof(ResidentInstance), cell_instances);
      pack.Add(CellPayloadPath(prefix, cell, Domain::kRepresentation, Tier::kFull),
               std::move(bytes));
      result->instances += cell_instances;
    }

    next_stable_id += stable_id_count;
    cell_begin = cell_end;
  }

  base::Vector<u8> index_bytes;
  if (!index.Encode(&index_bytes, &encode_error)) {
    SetError(error, encode_error);
    return false;
  }
  pack.Add(prefix + "/" + options.name + ".rxworld", std::move(index_bytes));

  if (!pack.WriteTo(archive_path)) {
    SetError(error, archive_path + ": cannot be written");
    return false;
  }
  result->bake_id = options.bake_id;
  return true;
}

}  // namespace rx::world
