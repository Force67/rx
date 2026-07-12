#ifndef RX_SCENE_COMPONENTS_H_
#define RX_SCENE_COMPONENTS_H_

#include <string>

#include "asset/asset_id.h"
#include "core/types.h"
#include "ecs/entity.h"

namespace rx::scene {

struct Transform {
  f32 position[3] = {0, 0, 0};
  f32 rotation[4] = {0, 0, 0, 1};  // quaternion
  f32 scale = 1.0f;
};

struct Renderable {
  asset::AssetId mesh;
};

// Tag marking an entity hidden; the render pass skips it. A tag carries no
// data, its presence is the state.
struct Hidden {};

// Packed rgb8 tint (0xRRGGBB) modulating the entity's albedo, 0 = untinted.
// Host::GatherEntityDraws copies it into the DrawItem, so apps can color
// otherwise-identical entities (teams, owners, debug states) per instance.
struct Tint {
  u32 rgb = 0;
};

// A human-readable label for an entity (editor outliner, scene text). Optional:
// unnamed entities simply lack it.
struct Name {
  std::string value;
};

// A stable identity independent of the ecs handle (which is reused after a
// destroy). Scene files reference entities by this, and the undo system tracks
// entities by it so a handle invalidated by destroy/recreate can be re-resolved.
// edit::EnsureGuid assigns a random nonzero value when absent.
struct Guid {
  u64 value = 0;
};

// A parent link. When present, the entity's Transform is expressed in the
// parent's local space rather than world space; edit::WorldTransform composes
// the chain. A parent-free entity has no Parent component and its Transform is
// world space, unchanged from before hierarchy existed.
struct Parent {
  ecs::Entity value{};
};

}  // namespace rx::scene

#endif  // RX_SCENE_COMPONENTS_H_
