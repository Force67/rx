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

// Another .rxscene instanced here, so a thing described once can be placed many
// times. This is the only reuse mechanism: `path` is a scene file, resolved
// RELATIVE TO THE FILE THAT NAMES IT rather than to the working directory
// (unlike Model.path and Surface.materialx, which name external art a scene
// merely points at). A prefab is scene content that belongs to the scene, so a
// scene directory has to stay movable as a unit.
//
// A .rxscene is the definition format rather than a second syntax because that
// buys every tool at once: a prefab file validates, renders and dumps its
// schema with the commands that already exist, and SaveScene round-trips an
// instance for free (see the expansion rule below). An inline `prefab` block
// would need its own parser in edit::LoadScene, and SaveScene, which writes
// entities, would drop it on the first live-edit save.
//
// The file's FIRST entity is the prefab's root: its components are added to
// this entity, skipping any this entity already authored. That skip is what
// lets one prefab serve many variants - the crate's Shape comes from the file,
// its Surface from the instance - and it is per COMPONENT, not per field: an
// instance that says anything about Surface owns the whole Surface.
//
// Every further entity in the file becomes a child of this entity, keeping its
// relative Transform so the authored Transform moves the whole group, and
// marked scene::Transient so SaveScene writes this Prefab line rather than the
// entities it expanded into. That is BuildSceneModels' treatment of a glTF's
// instances, for the same reason.
struct ScenePrefab {
  std::string path;
};

// Relative placement: stand this entity against another one instead of at a
// coordinate derived by hand. `target` is the other entity's Name.value and
// `mode` picks which side of it to sit against.
//
// The placement uses both entities' real world bounds, children included, so it
// resolves after BuildSceneShapes/BuildSceneModels rather than at load; a
// target whose geometry has no extent (a bare Light) cannot be measured and
// fails the load.
//
// An anchor REPLACES Transform.position: the anchor is the position, and the
// two axes the mode does not stack along centre on the target. It has to
// replace rather than offset, or a SaveScene of a running engine (which writes
// the resolved position along with the Anchor that produced it) would reload
// with the placement applied on top of itself.
//
// Anchors resolve in dependency order, so anchoring to something itself
// anchored works. A cycle fails the load naming the loop.
struct SceneAnchor {
  std::string target;
  std::string mode = "on";
};

// Regular repetition: a row or a grid, so N cells cost one declaration of the
// spacing instead of N stepped coordinates.
//
// One component, two roles. An entity with `count` and `step` is the CONTAINER:
// its Transform is the position of cell (0,0,0). An entity naming that
// container in `of` is a MEMBER: it becomes a child of the container and its
// Transform.position becomes the cell it lands in, taken in the order the file
// declares members, which is why a member needs no coordinate at all. Replaced,
// not added to, for the same reason an Anchor replaces one: a saved member
// carries the cell it was given, and a reload has to land it on the same cell
// rather than a cell further along. `cell` names a prefab (see ScenePrefab,
// same path resolution) that every member which does not instance one itself is
// an instance of, so the members stay down to the one thing that differs
// between them.
//
// Members are laid out before prefabs expand, so an entity a prefab expanded
// into cannot itself be a grid member: a grid is a layout of the entities the
// file declares.
struct SceneGrid {
  std::string of;
  std::string cell;
  // Cells along x, y and z. Fractional values truncate; the format has no
  // integer vector.
  f32 count[3] = {1, 1, 1};
  f32 step[3] = {0, 0, 0};
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

// The local-space bounds of an entity's built geometry, written by
// BuildSceneShapes and BuildSceneModels and read only by BuildSceneAnchors.
// Deliberately NOT reflected: it is derived from the mesh, so no scene authors
// it and no save should write it back. Presence means "this entity has
// something to measure"; an entity without it contributes nothing to a bound.
struct SceneBounds {
  f32 min[3] = {0, 0, 0};
  f32 max[3] = {0, 0, 0};
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

// Parents every SceneGrid member to the container it names, at the cell its
// declaration order earns it, and hands it the container's `cell` prefab when
// it instances none itself. Runs before BuildScenePrefabs, which is what stops
// an expanded entity from silently claiming a cell. `scene_path` is read only
// to turn a failure into the `path:line:` of the assignment that caused it.
//
// False + *error on a container that names nothing, a container with a
// non-positive cell count, a member that is its own container, and a grid asked
// to hold more members than it has cells - the last of which would otherwise
// stack two cells on one coordinate, which reads as a missing object.
bool BuildSceneGrids(ecs::World& world, const std::string& scene_path, std::string* error);

// Expands every ScenePrefab entity: merges the prefab root's components into it
// and adds one scene::Transient child per further entity of the prefab file
// (see ScenePrefab). Each file is loaded once however many entities name it.
// Runs before BuildSceneShapes, so the geometry a prefab carries is built like
// any other. `scene_path` locates both the error and the prefab, whose path is
// relative to the file naming it.
//
// False + *error on a prefab that does not load and on a prefab that reaches
// itself, which would otherwise expand until memory ran out.
bool BuildScenePrefabs(ecs::World& world, const std::string& scene_path, std::string* error);

// Resolves every SceneAnchor into a Transform.position, in dependency order so
// an anchor onto an anchored entity sees the settled one. Runs after
// BuildSceneShapes/BuildSceneModels because "on top of" is measured from the
// built geometry, not from the authored Shape.size.
//
// False + *error on a target that names no entity, a target no two entities
// agree on (two entities of that Name), a target with no geometry to measure,
// an unknown mode, and a cycle - each of which would otherwise leave an object
// at the origin, which reads as a scene that failed to place it.
bool BuildSceneAnchors(ecs::World& world, const std::string& scene_path, std::string* error);

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
