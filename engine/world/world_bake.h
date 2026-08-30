#ifndef RX_WORLD_WORLD_BAKE_H_
#define RX_WORLD_WORLD_BAKE_H_

#include <string>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "ecs/world.h"

// The cook: an authored .rxscene in, a streamable .rxp out.
//
// It lives here rather than in the rxworld tool because there is more than one
// front end - the tool and the editor - and two cooks would be two answers to
// what a world is. One function, called from both.
//
// The input is deliberately a path, not a live ecs::World, and that is not a
// convenience:
//
//   The bake id is a hash of the scene's bytes together with the cook settings
//   and the schema. A live-world cook would need a second definition of it, and
//   then the editor and the tool would stamp different ids on identical
//   content - exactly the disagreement that sharing the cook is meant to
//   prevent.
//
//   An editor's world is not its scene. It holds transient entities the author
//   never wrote: terrain-tile visuals, preview models. edit::SaveScene already
//   knows which of those to leave out, so cooking the file it wrote inherits
//   that judgement instead of duplicating it.
//
//   Stable id assignment is deterministic *because* the input is a file: the
//   cook loads it into a fresh ecs::World, so entity indices are file order,
//   and two cooks of one file replay the same creation sequence. Cooking a
//   lived-in world would order ties by that session's create/destroy history
//   and hand out different ids for identical content. This signature forbids
//   that caller structurally.
//
// The editor's flow is save, then bake what was saved.

namespace rx::world {

struct WorldBakeOptions {
  // Names the world, and the directory its index and payloads sit in inside the
  // archive: <name>/<name>.rxworld.
  std::string name = "world";
  f32 cell_size = 64.0f;
  // 0 hashes the cook: the scene's bytes, these settings, and the reflected
  // layout of every component actually written. An explicit value overrides it,
  // for a build that wants to stamp its own.
  u64 bake_id = 0;
  // Load the scene leniently, dropping components this build does not register.
  // Off by default: a cell that bakes without the thing it was authored to
  // place is worse than a cook that stops and says so.
  bool skip_unknown = false;
  // An entity whose authored component set is exactly this becomes an instance
  // page row rather than an ECS entity. Empty means Transform + Renderable.
  base::Vector<std::string> instance_components;
};

// What the cook makes of one entity.
enum class BakeRole : u8 {
  kEntity,    // an ECS row in the gameplay domain
  kInstance,  // a row in a representation instance page, with no ECS identity
  kRefused,   // the cook will not bake it at all
};

RX_WORLD_EXPORT const char* BakeRoleName(BakeRole role);

// What the cook would decide about one entity, without running a cook.
//
// The editor asks this for the current selection. The split between an entity
// and a page row is otherwise invisible until somebody inspects the archive,
// and an author who cannot see the split cannot author against it - which has
// already produced one real bug in this module's history.
struct BakeVerdict {
  BakeRole role = BakeRole::kEntity;
  // The cell it lands in, and that cell's lattice square on XZ. Absent when the
  // entity has no Transform, or one the lattice cannot hold.
  bool has_cell = false;
  u64 cell = 0;
  Vec3 cell_minimum;
  Vec3 cell_maximum;
  // Reflected components that cannot be baked and are dropped from it.
  base::Vector<std::string> dropped;
  // Why, when role is kRefused.
  std::string refusal;
};

RX_WORLD_EXPORT BakeVerdict ClassifyForBake(ecs::World& world, ecs::Entity entity,
                                            const WorldBakeOptions& options);

struct WorldBakeResult {
  u64 bake_id = 0;
  u32 cells = 0;
  u32 entities = 0;
  u32 instances = 0;
  // Components dropped because they cannot be restored by copying bytes, once
  // each. Not an error - a cook can legitimately carry them - but the author
  // has to be told, because every entity that had one no longer does.
  base::Vector<std::string> dropped;
};

// Cooks `scene_path` into a .rxp at `archive_path`. False, with `error` set to
// something naming the scene and the problem, on anything the cook refuses: a
// Parent link, an entity off the lattice, a component this build cannot bake or
// does not know, an empty scene, or a file it cannot read or write.
RX_WORLD_EXPORT bool BakeWorld(const std::string& scene_path, const WorldBakeOptions& options,
                               const std::string& archive_path, WorldBakeResult* result,
                               std::string* error);

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_BAKE_H_
