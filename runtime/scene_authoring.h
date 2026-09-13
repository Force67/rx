#ifndef RX_RUNTIME_SCENE_AUTHORING_H_
#define RX_RUNTIME_SCENE_AUTHORING_H_

#include <string>
#include <string_view>
#include <vector>

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
  // Image maps, each a path RELATIVE TO THE WORKING DIRECTORY (like materialx
  // and Model.path: a scene points at external art rather than owning it).
  // They LAYER ON the constants above, exactly as the glTF equivalent does:
  // base_color_map multiplies base_color, and so on, so a texture set wants
  // base_color = 1 1 1 to come through verbatim and an emissive_map is inert
  // while emissive is 0 0 0.
  //
  // A path that does not resolve FAILS THE LOAD naming the assignment; there is
  // no default texture, because a substituted checkerboard is a render that
  // looks authored and is not. Refused together with a Pattern on one entity
  // (BuildSceneShapes): both bind the same three material slots.
  std::string base_color_map;
  // Tangent space, OpenGL green-up (+y is +v), which is the convention the
  // engine's shaders and its own generated normal maps use. A DirectX-style map
  // (ambientCG ships both, _NormalDX and _NormalGL) lights inverted along v and
  // nothing here can tell the two apart, so pick the GL one.
  std::string normal_map;
  // Greyscale. Roughness and metallic are separate maps rather than one packed
  // ORM because that is how a CC0 texture set ships them; a scene wanting glTF
  // packing has the file's own material through Model.
  std::string roughness_map;
  std::string metallic_map;
  // Greyscale ambient occlusion, multiplying the INDIRECT light only. Direct
  // sun is unaffected, which is what keeps it from reading as painted-on dirt.
  std::string occlusion_map;
  std::string emissive_map;
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
// .gltf/.glb ships, with its materials and textures. The only component naming
// content the scene did not itself describe.
//
// `path` is the file, optionally narrowed with the "#mesh<index>" fragment
// ImportedScene already uses. The whole file places every instance its nodes
// describe as children of this entity (the authored Transform moves the lot);
// a "#mesh<index>" reference is placed at the entity's own Transform instead.
// Paths resolve against the working directory, like Surface.materialx.
//
// BuildSceneModels writes the Renderables, so a Model entity authors none. Not
// covered: skinning and animation (bind pose at the origin) and the file's own
// lights and cameras, which lose to the scene's.
struct SceneModel {
  std::string path;
};

// Another .rxscene instanced here: the only reuse mechanism. `path` resolves
// RELATIVE TO THE FILE THAT NAMES IT (prefabs are scene content and move with
// it), unlike Model.path / Surface.materialx which point at external art via
// the working directory. The format is a plain .rxscene so a prefab validates,
// renders and round-trips with the existing tools.
//
// The file's FIRST entity is the root: its components are added to this entity,
// skipping any it already authored, per COMPONENT (an instance saying anything
// about Surface owns the whole Surface), which is how one prefab serves many
// variants. Every further entity becomes a Transient child keeping its relative
// Transform, so the authored Transform moves the group and SaveScene writes
// this Prefab line instead of the expansion (same treatment as a glTF's
// instances).
struct ScenePrefab {
  std::string path;
};

// Orientation in degrees, because a quaternion is not something an author
// writes. `euler` is pitch, yaw, roll about the entity's OWN axes, applied
// q = Ry * Rx * Rz, which keeps yaw horizontal however the thing is pitched.
// Right-handed, y-up; positive yaw is counter-clockwise from above.
//
// REPLACES Transform.rotation rather than composing: BuildSceneRotations
// resolves it into the Transform and SaveScene writes the resolved quaternion
// beside it, so anything additive would turn the object again every round trip.
// A verbatim quaternion is authored as Transform.rotation with no Rotation.
// Per the usual per-component prefab rule (ScenePrefab): an instance authoring
// Rotation owns its orientation; authoring a raw Transform.rotation does NOT
// beat a prefab's Rotation (different components), and a live editor turning
// the entity by hand writes a quaternion the next load discards.
struct SceneRotation {
  f32 euler[3] = {0, 0, 0};
};

// Per-axis proportion for the entity's Shape: `scale` multiplies the built
// geometry on top of Shape.size (the only way to author an ellipsoid or
// flattened column, since every kind but box and plane reads size as radii).
// Its own component rather than a Shape prop because prefab merge is per
// COMPONENT (ScenePrefab): a `Shape.stretch` prop would let an instance that
// only wants different proportions replace the prefab's whole Shape.
//
// BAKED INTO THE VERTICES at build time, not carried on the Transform: the bake
// applies the inverse transpose to the normals once on cpu, so every downstream
// matrix stays a similarity. Two shapes differing only in stretch are two
// meshes (ShapeKey), so cost is per distinct proportion, not per entity. Every
// axis must be positive (the normal bake divides by them): zero is nans,
// negative is inside-out; BuildSceneShapes refuses naming the line, --validate
// reports degenerate_stretch.
//
// Scope is Shapes only (baking into imported geometry would cost a draw call
// per variant; stretch a Model by authoring it stretched). A MULTI-ENTITY
// PREFAB stretches whole: each part's offset and geometry scale with the
// instance's Stretch (a part's own Stretch is kept, multiplied through), so one
// podium + shaft + crown prefab yields as many silhouettes as it has instances.
//
// Refused: a NON-UNIFORM stretch of a prefab with a TURNED part (per-world-axis
// scaling is a shear on turned axes, which nothing here carries);
// BuildScenePrefabs fails naming the part. Uniform stretch is a similarity.
struct SceneStretch {
  f32 scale[3] = {1, 1, 1};
};

// Relative placement: stand this entity against another one's measured bounds
// (children included) instead of at a hand-derived coordinate. `target` is the
// other entity's Name.value; `mode` picks the side. Resolves after
// BuildSceneShapes/Models/Rotations, with both bounds measured rotated (every
// corner), so a tilted piece still sits on its plinth. A target with no extent
// (a bare Light) fails the load.
//
// An anchor REPLACES Transform.position (the anchor is the position; the two
// non-stacking axes centre on the target). Replace rather than offset, or a
// SaveScene round trip would apply the placement twice. `offset` is authored
// world-space displacement added to the solved position and survives the solve
// because it is input, unlike the position which is output: along the stacking
// axis a deliberate gap, across the others displacement from the target's
// centre (this is how anything sits on a ground plane without a hand-written y).
//
// Anchors resolve in dependency order, so anchoring to something itself
// anchored works; a cycle fails the load naming the loop.
struct SceneAnchor {
  std::string target;
  std::string mode = "on";
  f32 offset[3] = {0, 0, 0};
};

// Regular repetition: a row or a grid, so N cells cost one spacing declaration
// instead of N stepped coordinates.
//
// Two roles in one component. An entity with `count` and `step` is the
// CONTAINER (its Transform is cell (0,0,0)); an entity naming it in `of` is a
// MEMBER: a Transient child whose Transform.position is replaced by the cell it
// lands in, in declaration order (replaced, not added, so a save/load round
// trip lands it on the same cell). `cell` names a prefab (ScenePrefab path
// rules) instanced by every member that does not instance one itself.
// Members are laid out before prefabs expand, so expanded entities cannot be
// members: a grid lays out what the file declares.
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

// The key light, as an angle in the sky rather than a direction vector. Without
// it a scene is lit by whatever hour the world clock is at, and a punctual
// Light cannot stand in for a source at infinity. `elevation` is degrees above
// the horizon (negative is a set sun the sky darkens for); `azimuth` is degrees
// about y from +z, matching Rotation.euler's yaw.
//
// A scene declaring one takes the sun from the day/night clock ENTIRELY (a
// capture must be the same picture every run); a scene wanting the clock's sun
// authors no Sun at all. The first one declared wins, like the camera.
struct SceneSun {
  f32 elevation = 45.0f;
  f32 azimuth = 0.0f;
  f32 color[3] = {1.0f, 0.96f, 0.9f};
  f32 intensity = 4.0f;
  // Flat fill on everything the sun misses, so a shadowed face is dark rather
  // than black. The sky's own IBL is the rest of it.
  f32 ambient = 0.06f;
};

// The air between the camera and the thing it looks at, plus the exposure the
// result is developed at. Haze gives a big exterior its depth (without it every
// building renders at the same contrast at 10 m and 200 m) and rides the
// always-on froxel volume, so it is sun- and light-lit at no extra cost.
//
// `density` is base scattering per metre (0.005 subtle, 0.02 misty street, 0.1
// opaque fog); `start_distance` is metres of clear air before the ramp, which
// keeps an interior's near field crisp while still getting shafts. `exposure`
// multiplies the auto-exposure result: a stop-style nudge, 1 leaves metering
// alone.
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
// Also on a Surface texture map that names no image, and on an entity carrying
// both a Pattern and a Surface texture map (see SceneSurface).
bool BuildSceneShapes(ecs::World& world, asset::AssetDatabase& db, render::Renderer* renderer,
                      const std::string& scene_path, std::string* error);

// Why a Surface texture-map path names no image that can be bound, or empty
// when it does. Only the header is read, so this costs no decode; the point is
// that BuildSceneShapes and --validate go through one function, and neither can
// accept a map the other would reject. Same role SceneModelProblem plays for
// Model.path.
std::string SceneSurfaceMapProblem(const std::string& path);

// One Surface prop that names an image, and what this surface writes in it.
struct SceneSurfaceMapRef {
  const char* prop;         // "base_color_map", "normal_map", ...
  const std::string* path;  // the authored value, empty when the prop is unset
};

// Every texture-map prop of `surface`, set or not, in declaration order. One
// list so the mesh key, the loader, the Pattern conflict and --validate cannot
// disagree about which props are maps: a map added to SceneSurface and not to
// the table behind this drops out of ShapeKey, and two surfaces differing only
// in it then collide onto one material.
std::vector<SceneSurfaceMapRef> SceneSurfaceMaps(const SceneSurface& surface);

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
