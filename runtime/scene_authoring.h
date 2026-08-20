#ifndef RX_RUNTIME_SCENE_AUTHORING_H_
#define RX_RUNTIME_SCENE_AUTHORING_H_

#include <string>
#include <string_view>

#include "asset/asset_database.h"
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

// Procedural geometry. `size` means something different per kind (see the Hint
// registered for it), because one vec3 covers half extents, a radius, and the
// radius pairs a torus and a capsule need. BuildSceneShapes writes the built
// mesh into the entity's Renderable, so an entity does not author one itself.
struct SceneShape {
  std::string kind = "box";
  f32 size[3] = {0.5f, 0.5f, 0.5f};
};

// The material for the entity's SceneShape. Only lobes the mesh shaders
// actually consume are here: everything below visibly changes the render, and
// the ones that need the sun rather than a point light say so in their hints.
// Without a Surface the shape gets the engine's default material.
struct SceneSurface {
  f32 base_color[3] = {0.8f, 0.8f, 0.8f};
  f32 roughness = 0.6f;
  f32 metallic = 0.0f;
  f32 emissive[3] = {0, 0, 0};
  // Clear lacquer over the base layer: car paint, varnished wood, wet stone.
  f32 clearcoat = 0.0f;
  f32 clearcoat_roughness = 0.0f;
  // Stretches the specular highlight along the surface tangent (brushed metal,
  // hair). Negative stretches the other way.
  f32 anisotropy = 0.0f;
  // Dielectric index of refraction, which is what sets the specular level of a
  // non-metal. 1.5 is glass/plastic, 1.33 water, 1.8 gemstone.
  f32 ior = 1.5f;
  // Retroreflective fuzz at grazing angles: velvet, felt, dusty cloth.
  f32 sheen_color[3] = {0, 0, 0};
  f32 sheen_roughness = 0.3f;
  // Wrap lighting plus back-scatter through thin geometry (wax, leaves, skin).
  // subsurface_color also tints the three light fills below.
  f32 subsurface_color[3] = {0.9f, 0.3f, 0.2f};
  f32 subsurface = 0.0f;
  // Thin-film interference on the specular: soap bubbles, oil, beetle shells.
  f32 iridescence = 0.0f;
  f32 iridescence_thickness = 400.0f;
  // Refracts what is behind the surface instead of diffusing. Anything above 0
  // moves the shape into the sorted transparent pass.
  f32 transmission = 0.0f;
  // Tint and level of the direct specular lobe, independent of base_color.
  // strength 0 is matte no matter how low the roughness.
  f32 specular_color[3] = {1, 1, 1};
  f32 specular_strength = 1.0f;
  // Fresnel-weighted reflection of the engine's own environment, layered over
  // the base material. This is how armour, ice and gems shine without being
  // metal.
  f32 env_reflect = 0.0f;
  // The wrap-around light fills: soft spills the key light past the terminator,
  // rim rides the edge of a backlit surface (the value is its falloff
  // exponent), back transmits straight through. All tint by subsurface_color.
  f32 soft_lighting = 0.0f;
  f32 rim_lighting = 0.0f;
  f32 back_lighting = 0.0f;
  // A MaterialX document to take the whole material from instead of the fields
  // above. Relative to the working directory; a document that will not load
  // fails the scene load rather than falling back to the authored values.
  std::string materialx;
};

// A procedural texture bound to the entity's SceneSurface, generated at load.
// One pattern drives the base colour map and, when asked for, the normal and
// roughness maps, so a surface's relief always describes the same bricks its
// albedo does. Without one the surface is a flat colour.
struct ScenePattern {
  std::string kind = "checker";
  // Cells across the shape's uv square. Every primitive's uv covers 0..1 once,
  // so this is the only thing that decides how often the pattern repeats.
  f32 scale = 4.0f;
  // The two ends of the pattern. These MULTIPLY Surface.base_color the way a
  // glTF base-colour texture does, so leave that at 1 1 1 to get them verbatim.
  f32 color_a[3] = {0.25f, 0.25f, 0.25f};
  f32 color_b[3] = {0.8f, 0.8f, 0.8f};
  // Grid line / brick mortar width, as a fraction of one cell.
  f32 line_width = 0.08f;
  u32 seed = 0;
  u32 resolution = 256;
  // Depth of the relief the generated normal map fakes, in uv units. 0 binds no
  // normal map at all.
  f32 relief = 0.0f;
  // Roughness at the two ends of the pattern, MULTIPLYING Surface.roughness the
  // same way the colours multiply base_color. Equal values bind no map.
  f32 roughness_a = 1.0f;
  f32 roughness_b = 1.0f;
};

// Geometry from a real art asset instead of a primitive: the meshes a
// .gltf/.glb file ships, with the materials and textures it ships with. This is
// the only component that names content the scene did not itself describe.
//
// `path` is the file, optionally narrowed to one of its meshes by the same
// "#mesh<index>" fragment ImportedScene already addresses its assets by:
//
//   Model.path = "test/data/AnimatedMorphCube.glb"        the whole file
//   Model.path = "test/data/AnimatedMorphCube.glb#mesh0"  one mesh of it
//
// The whole file places every instance its nodes describe, as children of this
// entity, so the file keeps its internal layout and the authored Transform
// moves the lot. A "#mesh<index>" reference is placed at the entity's Transform
// instead, ignoring wherever the file's nodes put it: naming one mesh out of a
// library file means "put THIS one here". Relative paths resolve against the
// working directory, like Surface.materialx.
//
// BuildSceneModels writes the Renderables, so a Model entity authors none. Not
// covered: skinning and animation (a skinned mesh draws in its bind pose at the
// origin, since glTF has the node ignore its own transform), and the file's own
// lights and cameras, which lose to the scene's.
struct SceneModel {
  std::string path;
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

// Which axes of a kind's Shape.size must be positive for the primitive to
// enclose any volume, as a bitmask (bit 0 = x, 1 = y, 2 = z). Zero means no
// primitive builds `kind` at all. This is narrower than the axes a kind READS:
// a capsule of half height 0 is a legal sphere, so only its radius is required.
// BuildSceneShapes and --validate both go through here, so neither can accept a
// kind, or condemn a size, the other would not.
u32 ShapeRequiredSizeAxes(std::string_view kind);

// Builds one mesh + material per SceneShape entity, uploads them (`renderer`
// null skips the GPU side) and points each entity's Renderable at the result.
// Generated pattern textures also go into `db`, which is where an asset a scene
// synthesized belongs and what lets a headless build inspect them. Shapes that
// agree on every field share one mesh. False + *error on an unrecognized
// SceneShape::kind or ScenePattern::kind, or a MaterialX document that will not
// load, all of which would otherwise silently place a grey box where the author
// asked for something else.
bool BuildSceneShapes(ecs::World& world, asset::AssetDatabase& db, render::Renderer* renderer,
                      std::string* error);

// Why a Model.path names no geometry that can be placed, or empty when it does:
// the clause a caller puts behind a `path:line:`. Imports the file to answer,
// because nothing short of that can tell a real file from a plausible name, or
// know how many meshes are in it. BuildSceneModels and --validate both go
// through here, so neither can accept a reference the other would reject.
std::string SceneModelProblem(const std::string& path);

// Imports every Model entity's file (once per file, however many entities name
// it), publishes its meshes, materials and textures into `db` and onto the gpu
// (`renderer` null skips the gpu side), and points the entity - or one child
// entity per instance the file places - at the result. `scene_path` is read
// only to turn a failure into the `path:line:` of the assignment that caused
// it. False + *error on a reference that resolves to nothing, which would
// otherwise leave a hole exactly where the author asked for a model.
bool BuildSceneModels(ecs::World& world, asset::AssetDatabase& db, render::Renderer* renderer,
                      const std::string& scene_path, std::string* error);

}  // namespace rx

#endif  // RX_RUNTIME_SCENE_AUTHORING_H_
