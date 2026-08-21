#ifndef RX_RUNTIME_SCENE_AUTHORING_H_
#define RX_RUNTIME_SCENE_AUTHORING_H_

#include <string>
#include <string_view>

#include "asset/asset_database.h"
#include "core/types.h"
#include "ecs/world.h"
#include "render/core/renderer.h"
#include "render/core/settings.h"

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
//
// Only box and plane read all three axes of `size`; the per-axis proportion
// every other kind lacks is SceneStretch, which is its own component so that it
// composes with a prefab's Shape instead of replacing it.
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
  // Cells across the shape's uv square, per axis: [0] along u, [1] along v.
  // Every primitive's uv covers 0..1 once, so this is the only thing that
  // decides how often the pattern repeats.
  //
  // Both axes because a facade is "5 bays across and 6 floors up" and a box is
  // rarely a cube: with one number the cells take the face's aspect ratio, so a
  // 6x14 tower gets windows two and a half times taller than they are wide,
  // whoever authored it. Both axes have to be positive - the short list this
  // format pads with zeros is, here, an author who wrote one number for a prop
  // that needs two, and that reads as a wall of stripes rather than as a
  // mistake, so BuildSceneShapes refuses it by name.
  f32 scale[2] = {4.0f, 4.0f};
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

// Orientation in degrees, because a quaternion is not something an author (or
// an agent) writes down: "turn this 30 degrees about y" is "0 30 0" here and
// four hand-computed numbers in Transform.rotation, which is why every scene
// authored before this component was axis-aligned.
//
// `euler` is degrees about x, y and z - pitch, yaw and roll - applied yaw, then
// pitch, then roll about the entity's OWN axes (q = Ry * Rx * Rz). That order is
// what keeps yaw horizontal however the thing is pitched, which is what a
// placement almost always means. Right-handed and y-up like the rest of the
// engine, so a positive yaw turns counter-clockwise seen from above and y = 90
// takes the entity's +z face onto +x.
//
// It REPLACES Transform.rotation rather than composing onto it, for the same
// reason an Anchor replaces Transform.position: BuildSceneRotations resolves it
// into the Transform, and SaveScene writes the resolved quaternion next to the
// Rotation that produced it, so anything additive would turn the object again on
// every save/load round trip. An entity wanting a quaternion verbatim authors
// Transform.rotation and no Rotation, and a live editor sees the failure mode an
// Anchor already has: turning the entity by hand writes a quaternion the next
// load throws away for the euler beside it.
//
// Against a prefab it follows the usual per-component rule (see ScenePrefab): an
// instance that authors Rotation owns its orientation, one that does not takes
// the prefab's. An instance authoring a raw Transform.rotation does NOT beat a
// prefab's Rotation, because the two are different components; author Rotation
// on both sides, or neither.
struct SceneRotation {
  f32 euler[3] = {0, 0, 0};
};

// Per-axis proportion for the entity's Shape: `scale` multiplies the built
// geometry along x, y and z, on top of whatever Shape.size the kind read. This
// is the only way to say "an ellipsoid", "an oval torus" or "a flattened
// column", since every kind but box and plane reads size as radii.
//
// Its own component rather than a Shape prop for the reason SceneRotation is
// one: prefab merge is per COMPONENT (see ScenePrefab), so a `Shape.stretch`
// would make an instance that only wants different proportions replace the
// prefab's whole Shape, silently losing the kind and size it meant to keep -
// one building prefab at three proportions is the case this exists for, and it
// costs one line beside the Prefab.path:
//
//   Transform.position = 12 18 0
//   Stretch.scale = 1.6 0.7 1
//   Prefab.path = "prefabs/city/tower_glass.rxscene"
//
// It is BAKED INTO THE VERTICES at build time rather than carried on the
// Transform, which is what keeps it free: the bake applies the inverse
// transpose to the normals once, on the cpu, exactly, so every matrix
// downstream stays a similarity and no shader, bound or transform path has to
// know the mesh was stretched. Two shapes differing only in the stretch are two
// meshes (see ShapeKey), and two entities agreeing on it share one, so the cost
// is per distinct proportion rather than per entity.
//
// Every axis has to be positive: the normal bake divides by them, so a zero is a
// mesh of nans and a negative one is a mesh turned inside out. BuildSceneShapes
// refuses the load naming the line, and --validate reports it as
// degenerate_stretch.
//
// Scope is Shapes, and only Shapes. A Model is deliberately not stretched:
// baking into imported geometry means a vertex copy and a mesh id per stretch
// value, which is a draw call per variant instead of per instance, so a glTF
// asset is stretched by authoring it stretched.
//
// A MULTI-ENTITY PREFAB stretches whole. The instance's Stretch reaches every
// entity the prefab expanded into: each part's offset from the instance scales
// with it, so a crown ten metres up stays on top of a shaft made twice as tall,
// and each part's own geometry multiplies by it, so the crown widens when the
// building does. A part that authored its own Stretch keeps it, multiplied
// through rather than replaced.
//
// That is what makes proportion and SILHOUETTE independent. A building authored
// as podium + shaft + crown is one prefab and as many outlines as there are
// instances of it; without it a stretched prefab came back with a stretched
// root and its other parts floating at their authored size, so the only shape
// that survived being stretched was a single box - which is why an authored
// city used to be rectangles of differing height and nothing else.
//
// The one case it refuses: a NON-UNIFORM stretch of a prefab with a TURNED
// part. Scaling per world axis is only a scale while the part's axes agree with
// the world's; on a turned one it is a shear, which no mesh, bound or transform
// in this engine can carry. BuildScenePrefabs fails the load naming the part.
// A uniform stretch is a similarity and is always allowed.
struct SceneStretch {
  f32 scale[3] = {1, 1, 1};
};

// Relative placement: stand this entity against another one instead of at a
// coordinate derived by hand. `target` is the other entity's Name.value and
// `mode` picks which side of it to sit against.
//
// The placement uses both entities' real world bounds, children included, so it
// resolves after BuildSceneShapes/BuildSceneModels rather than at load; a
// target whose geometry has no extent (a bare Light) cannot be measured and
// fails the load. It also runs after BuildSceneRotations, because a turned box
// stands on a different footprint than an axis-aligned one: both boxes are
// measured with their rotation applied (every corner, not just min and max), so
// a piece tilted on its plinth still sits on it rather than through or above it.
//
// An anchor REPLACES Transform.position: the anchor is the position, and the
// two axes the mode does not stack along centre on the target. It has to
// replace rather than offset, or a SaveScene of a running engine (which writes
// the resolved position along with the Anchor that produced it) would reload
// with the placement applied on top of itself.
//
// `offset` is how an author still says where, in world axes, added to the
// solved position. Centring is right for a plant room on a tower and useless
// for the far commoner case of standing something on the GROUND: without an
// offset every object on a floor plane lands on the same spot, so the whole
// scene goes back to hand-written coordinates, and the y in them is a hand-run
// multiplication of the prefab's half height by its Stretch that silently
// sinks the object the moment either changes. With one, "on the ground at
// x -15, z 16" is Anchor.target = "Ground", mode = "on", offset = -15 0 16 -
// and the height, the only number that was ever derived, stays derived.
//
// It survives the solve because it is authored INPUT, unlike Transform.position
// which is the solve's output: the round trip re-derives the position from the
// bounds and this, so it lands in the same place however many times it is
// saved and reloaded. Along the stacking axis it is a deliberate gap or bite;
// across the other two it is displacement from the target's centre.
//
// Anchors resolve in dependency order, so anchoring to something itself
// anchored works. A cycle fails the load naming the loop.
struct SceneAnchor {
  std::string target;
  std::string mode = "on";
  f32 offset[3] = {0, 0, 0};
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

// The key light, as an angle in the sky rather than a direction vector.
//
// Without this a scene is lit by whatever hour the world clock happens to be
// at, which is the single largest thing deciding what a render looks like and
// was the one thing a text scene could not say: an author could place every
// object to the centimetre and still not ask for the light to come from the
// left. Placing it takes a SUN, not a Light - a punctual light with a radius
// cannot stand in for a source at infinity, and a scene that tried lit its
// facades from a point halfway up the street.
//
// `elevation` is degrees above the horizon (90 is overhead, 0 is on it, and
// below 0 is a set sun the sky darkens for) and `azimuth` is degrees about y
// from +z, counter-clockwise seen from above, matching Rotation.euler's yaw.
// Both in degrees for the reason SceneRotation is: an author writes "low sun
// from behind the towers", not a normalized triple.
//
// A scene declaring one takes the sun over from the day/night clock ENTIRELY,
// so the clock no longer moves it. That is the point - a capture has to be the
// same picture on every run - but it means a scene wanting the clock's sun
// authors no Sun at all rather than a Sun it hopes matches.
//
// The first one the scene declares wins, like the camera.
struct SceneSun {
  f32 elevation = 45.0f;
  f32 azimuth = 0.0f;
  f32 color[3] = {1.0f, 0.96f, 0.9f};
  f32 intensity = 4.0f;
  // Flat fill on everything the sun misses, so a shadowed face is dark rather
  // than black. The sky's own IBL is the rest of it.
  f32 ambient = 0.06f;
};

// The air between the camera and the thing it is looking at, plus the exposure
// the result is developed at.
//
// Haze is what gives a big exterior its depth: without it every building is
// rendered at the same contrast whether it is 10 or 200 metres away, which is
// most of why a flat blockout reads as a diagram rather than as a place. It
// rides the always-on froxel volume, so it is lit by the sun and by every
// punctual light in the scene and costs nothing extra to ask for.
//
// `density` is the base scattering per metre (0.005 is the engine's own subtle
// haze, 0.02 is a visibly misty street, 0.1 is fog you cannot see through) and
// `start_distance` is metres of clear air before it ramps in, which is how an
// interior keeps its near field crisp while still getting shafts across the
// room. `exposure` multiplies the auto-exposure result, so it is a stop-style
// nudge rather than an absolute: 1 leaves the metering alone.
struct SceneAtmosphere {
  f32 density = 0.005f;
  f32 start_distance = 0.0f;
  f32 exposure = 1.0f;
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

// Resolves every SceneRotation into its entity's Transform.rotation, adding a
// Transform to an entity that has none. Runs after BuildScenePrefabs, so a
// rotation a prefab carries resolves like an authored one, and before
// BuildSceneShapes/BuildSceneAnchors, which measure the turned geometry.
//
// No failure path: every finite triple of degrees is a legal orientation, and a
// non-finite one is refused by the loader with the line that wrote it.
void BuildSceneRotations(ecs::World& world);

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
// agree on every field share one mesh. A SceneStretch other than 1 1 1 is baked
// into that mesh here (see SceneStretch), so the SceneBounds this writes are the
// stretched ones and an anchor onto a stretched object needs no special case.
// `scene_path` is read only to turn a failure into the `path:line:` of the
// assignment that caused it.
//
// False + *error on an unrecognized SceneShape::kind or ScenePattern::kind, or a
// MaterialX document that will not load, all of which would otherwise silently
// place a grey box where the author asked for something else, and on a
// Stretch.scale with a non-positive axis, which the normal bake would divide by.
bool BuildSceneShapes(ecs::World& world, asset::AssetDatabase& db, render::Renderer* renderer,
                      const std::string& scene_path, std::string* error);

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

// Writes the first SceneSun and SceneAtmosphere the scene declares into
// `settings`, and returns whether a Sun was among them - which the caller has
// to take as "stop driving the sun from the clock", or the next frame moves it
// back and the capture stops being reproducible.
//
// Both are optional and independent: a scene with neither is left on the
// engine's defaults, and one with only an Atmosphere still gets the clock's
// sun. Nothing here can fail, because there is no value in range that produces
// no picture; --validate carries the warnings about ones that produce a bad
// one.
bool ApplySceneEnvironment(ecs::World& world, render::RenderSettings* settings);

}  // namespace rx

#endif  // RX_RUNTIME_SCENE_AUTHORING_H_
