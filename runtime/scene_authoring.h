#ifndef RX_RUNTIME_SCENE_AUTHORING_H_
#define RX_RUNTIME_SCENE_AUTHORING_H_

#include <string>

#include "core/types.h"
#include "ecs/world.h"
#include "render/core/renderer.h"

// What a hand-authored .rxscene may carry on top of the engine's builtin
// components (Transform, Name, Guid, Parent, ...). A text scene has no binary
// assets to point a Renderable at and no way to describe light or viewpoint, so
// those are written inline and turned into engine assets at load.
//
// They live in the runtime rather than engine/scene because materializing them
// is viewer policy: the engine has no opinion on where a mesh comes from. Being
// reflected, they document themselves through --dump-schema.
namespace rx {

// Procedural geometry. `size` is the half extents for a box and (radius, -, -)
// for a sphere. BuildSceneShapes writes the built mesh into the entity's
// Renderable, so an entity does not author one itself.
struct SceneShape {
  std::string kind = "box";
  f32 size[3] = {0.5f, 0.5f, 0.5f};
};

// Metallic-roughness surface for the entity's SceneShape. Without one the shape
// gets the engine's default material.
struct SceneSurface {
  f32 base_color[3] = {0.8f, 0.8f, 0.8f};
  f32 roughness = 0.6f;
  f32 metallic = 0.0f;
  f32 emissive[3] = {0, 0, 0};
};

// A punctual light at the entity's Transform position. `radius` is the
// influence cutoff in meters, past which the light contributes nothing.
struct SceneLight {
  f32 color[3] = {1, 1, 1};
  f32 intensity = 4.0f;
  f32 radius = 6.0f;
};

// The viewpoint: the entity's Transform position is the eye, `target` the point
// it looks at. The first one the scene declares wins.
struct SceneCamera {
  f32 target[3] = {0, 0, 0};
  f32 fov_degrees = 60.0f;
};

// Registers the components above with the edit reflection registry, so
// LoadScene resolves them by name and --dump-schema documents them. Idempotent;
// must run before either.
void RegisterSceneComponents();

// Builds one mesh + material per SceneShape entity, uploads them (`renderer`
// null skips the GPU side) and points each entity's Renderable at the result.
// Shapes that agree on every field share one mesh. False + *error on an
// unrecognized SceneShape::kind, which would otherwise silently place a box
// where the author asked for something else.
bool BuildSceneShapes(ecs::World& world, render::Renderer* renderer, std::string* error);

}  // namespace rx

#endif  // RX_RUNTIME_SCENE_AUTHORING_H_
