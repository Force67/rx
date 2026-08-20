#include "scene_authoring.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "asset/gltf_loader.h"
#include "asset/materialx.h"
#include "asset/primitives.h"
#include "asset/procedural_texture.h"
#include "asset/scene_import.h"
#include "asset/vfs.h"
#include "core/math.h"
#include "edit/hierarchy.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "scene/components.h"

namespace rx {
namespace {

// The numeric fields of each authoring component form one contiguous block of
// f32/u32 (no padding can creep in between equally aligned members), so the key
// below hashes the block instead of listing thirty fields by hand. INVARIANT: a
// new numeric field goes between the two members named per component, or it
// drops out of the key and two surfaces differing only in it collide onto one
// material.
constexpr size_t kSurfaceKeyOffset = offsetof(SceneSurface, base_color);
constexpr size_t kSurfaceKeyBytes =
    offsetof(SceneSurface, back_lighting) + sizeof(f32) - kSurfaceKeyOffset;
constexpr size_t kPatternKeyOffset = offsetof(ScenePattern, scale);
constexpr size_t kPatternKeyBytes =
    offsetof(ScenePattern, roughness_b) + sizeof(f32) - kPatternKeyOffset;

// The layout notices nothing when that invariant is broken: both components end
// in tail padding (the std::string forces 8-byte alignment) that an appended f32
// lands in, leaving every offsetof and sizeof exactly as they were. Counting the
// fields is what notices. Braces elide into the f32[N] members, so these are
// counts of numbers, not of declarations.
struct AnyField {
  template <typename T>
  operator T() const;
};
template <typename T, typename... F>
constexpr size_t FieldCount() {
  if constexpr (requires { T{F{}..., AnyField{}}; }) {
    return FieldCount<T, F..., AnyField>();
  } else {
    return sizeof...(F);
  }
}
static_assert(FieldCount<SceneSurface>() == 32,
              "SceneSurface changed: widen kSurfaceKeyBytes to cover the new field before bumping "
              "this, or two surfaces differing only in it collide onto one material");
static_assert(FieldCount<ScenePattern>() == 14,
              "ScenePattern changed: widen kPatternKeyBytes to cover the new field before bumping "
              "this, or two patterns differing only in it collide onto one texture");

// Numbers go into the key as raw bytes rather than formatted: MakeAssetId
// hashes the whole span, and a human-readable float would collide across values
// that happen to print the same.
void AppendBytes(std::string& key, const void* data, size_t bytes) {
  key.append(static_cast<const char*>(data), bytes);
}

// Every field that changes a built asset, so two entities share a mesh and a
// material exactly when sharing them is correct.
std::string ShapeKey(const SceneShape& shape, const SceneSurface& surface,
                     const ScenePattern* pattern) {
  std::string key = "rxscene/" + shape.kind + "/" + surface.materialx + "/";
  AppendBytes(key, shape.size, sizeof(shape.size));
  AppendBytes(key, reinterpret_cast<const char*>(&surface) + kSurfaceKeyOffset, kSurfaceKeyBytes);
  if (pattern) {
    key += "/" + pattern->kind + "/";
    AppendBytes(key, reinterpret_cast<const char*>(pattern) + kPatternKeyOffset,
                kPatternKeyBytes);
  }
  return key;
}

void ApplySurface(const SceneSurface& surface, asset::Material* material) {
  for (int i = 0; i < 3; ++i) {
    material->base_color_factor[i] = surface.base_color[i];
    material->emissive_factor[i] = surface.emissive[i];
    material->sheen_color[i] = surface.sheen_color[i];
    material->subsurface_color[i] = surface.subsurface_color[i];
    material->specular_color[i] = surface.specular_color[i];
  }
  material->roughness_factor = surface.roughness;
  material->metallic_factor = surface.metallic;
  material->clearcoat = surface.clearcoat;
  material->clearcoat_roughness = surface.clearcoat_roughness;
  material->anisotropy = surface.anisotropy;
  material->ior = surface.ior;
  material->sheen_roughness = surface.sheen_roughness;
  material->subsurface = surface.subsurface;
  material->iridescence = surface.iridescence;
  material->iridescence_thickness = surface.iridescence_thickness;
  material->transmission = surface.transmission;
  material->specular_strength = surface.specular_strength;
  material->env_reflect = surface.env_reflect;
  material->soft_lighting = surface.soft_lighting;
  material->rim_lighting = surface.rim_lighting;
  material->back_lighting = surface.back_lighting;
}

// Synthesizes the pattern's maps, binds them to `material` and hands them to
// the db and the gpu. False + *error on a pattern name nothing generates.
bool ApplyPattern(const ScenePattern& pattern, const std::string& key, asset::AssetDatabase& db,
                  render::Renderer* renderer, asset::Material* material, std::string* error) {
  asset::PatternDesc desc;
  if (!asset::ParsePatternKind(pattern.kind, &desc.kind)) {
    if (error)
      *error = "unknown Pattern.kind '" + pattern.kind +
               "' (checker | grid | brick | gradient | noise)";
    return false;
  }
  desc.width = desc.height = std::clamp(pattern.resolution, 4u, 2048u);
  desc.scale = pattern.scale;
  desc.line_width = pattern.line_width;
  desc.seed = pattern.seed;

  // Order matters: MaterialSystem resolves a material's texture slots at
  // UploadMaterial and silently falls back to its 1x1 defaults for anything not
  // uploaded yet, so every map has to reach the gpu before the material does.
  auto publish = [&](asset::Texture texture) {
    if (renderer) renderer->UploadTexture(texture);
    db.AddTexture(std::move(texture));
  };

  asset::Texture color = asset::MakePatternTexture(desc, pattern.color_a, pattern.color_b,
                                                   /*srgb=*/true,
                                                   asset::MakeAssetId(key + "/base_color"));
  material->base_color = color.id;
  publish(std::move(color));

  if (pattern.relief > 0.0f) {
    asset::Texture normal = asset::MakePatternNormalMap(desc, pattern.relief,
                                                        asset::MakeAssetId(key + "/normal"));
    material->normal = normal.id;
    publish(std::move(normal));
  }
  if (pattern.roughness_a != pattern.roughness_b) {
    asset::Texture mr = asset::MakePatternRoughnessMap(desc, pattern.roughness_a,
                                                       pattern.roughness_b,
                                                       asset::MakeAssetId(key + "/roughness"));
    material->metallic_roughness = mr.id;
    publish(std::move(mr));
  }
  return true;
}

// The size axes each kind needs positive; see ShapeRequiredSizeAxes.
struct ShapeKind {
  const char* name;
  u32 required_size_axes;
};
constexpr ShapeKind kShapeKinds[] = {
    {"box", 0b111},  {"sphere", 0b001}, {"plane", 0b101},  {"cylinder", 0b011},
    {"cone", 0b011}, {"torus", 0b011},  {"capsule", 0b001},
};

// False + *error on a kind no primitive builds, which would otherwise put a box
// where the author asked for something else.
bool BuildShapeMesh(const SceneShape& shape, asset::AssetId id, asset::Mesh* out,
                    std::string* error) {
  // The table gates the chain rather than the other way round, so a kind added
  // below but not above fails loudly here instead of building a shape
  // --validate would then reject as unknown.
  if (ShapeRequiredSizeAxes(shape.kind) == 0) {
    if (error)
      *error = "unknown Shape.kind '" + shape.kind +
               "' (box | sphere | plane | cylinder | cone | torus | capsule)";
    return false;
  }
  const f32* size = shape.size;
  if (shape.kind == "box") {
    *out = asset::MakeBox(size[0], size[1], size[2], id);
  } else if (shape.kind == "sphere") {
    *out = asset::MakeSphere(size[0], 24, 32, id);
  } else if (shape.kind == "plane") {
    *out = asset::MakePlane(size[0], size[2], id);
  } else if (shape.kind == "cylinder") {
    *out = asset::MakeCylinder(size[0], size[1], 32, id);
  } else if (shape.kind == "cone") {
    *out = asset::MakeCone(size[0], size[1], 32, id);
  } else if (shape.kind == "torus") {
    *out = asset::MakeTorus(size[0], size[1], 24, 40, id);
  } else if (shape.kind == "capsule") {
    *out = asset::MakeCapsule(size[0], size[1], 16, 32, id);
  } else {
    // Unreachable while the table and this chain agree. A kind added to one and
    // not the other lands here rather than quietly building the wrong shape.
    if (error) *error = "Shape.kind '" + shape.kind + "' has no primitive builder";
    return false;
  }
  return true;
}

// The mesh's local axis-aligned bounds, from lod 0's positions. False for a
// mesh with no geometry, which then carries no SceneBounds and so cannot be
// anchored against. Mesh::bounds_radius is deliberately not used: it is a
// sphere, and "on top of" a box asked from a sphere floats the object by the
// difference between a corner and a face.
bool MeshBounds(const asset::Mesh& mesh, SceneBounds* out) {
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) return false;
  const base::Vector<asset::Vertex>& vertices = mesh.lods[0].vertices;
  for (u32 axis = 0; axis < 3; ++axis) {
    out->min[axis] = out->max[axis] = vertices[0].position[axis];
  }
  for (const asset::Vertex& vertex : vertices) {
    for (u32 axis = 0; axis < 3; ++axis) {
      out->min[axis] = std::min(out->min[axis], vertex.position[axis]);
      out->max[axis] = std::max(out->max[axis], vertex.position[axis]);
    }
  }
  return true;
}

// Splits a Model.path into the file to import and the mesh it selects, *index
// staying -1 without a fragment. The fragment spelling is ImportedScene's own
// ("<path>#mesh<index>"), so what a scene writes and what the importer names
// its assets are the same convention rather than two that have to be kept in
// step. Returns the clause saying why the path is not addressable, or empty.
std::string SplitModelPath(const std::string& path, std::string* file, i32* index) {
  *file = path;
  *index = -1;
  if (path.empty()) return "is empty; there is nothing to place";

  const size_t hash = path.rfind('#');
  if (hash != std::string::npos) {
    const std::string_view fragment = std::string_view(path).substr(hash);
    const std::string_view digits = fragment.substr(std::min<size_t>(fragment.size(), 5));
    if (fragment.compare(0, 5, "#mesh") != 0 || digits.empty() ||
        digits.find_first_not_of("0123456789") != std::string_view::npos) {
      return std::format("has fragment '{}'; only '#mesh<N>' selects one mesh of a file",
                         fragment);
    }
    *index = static_cast<i32>(std::strtol(std::string(digits).c_str(), nullptr, 10));
    *file = path.substr(0, hash);
  }

  std::string extension;
  if (const size_t dot = file->rfind('.'); dot != std::string::npos) {
    for (char c : std::string_view(*file).substr(dot))
      extension += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (extension != ".gltf" && extension != ".glb") {
    return "is not a .gltf or .glb; a usd stage loads through --usd, which has its own importer";
  }
  return {};
}

// Why the selection a Model.path makes cannot be honoured by the file that was
// actually imported, or empty. Split out so the message is written once and
// both a fresh import and an already-imported file are judged by it.
std::string SelectionProblem(i32 index, size_t meshes, size_t instances) {
  if (index >= 0) {
    if (static_cast<size_t>(index) >= meshes) {
      return std::format("selects mesh {} of a file that has {}", index, meshes);
    }
    return {};
  }
  if (instances == 0) return "places nothing: no node in the file carries a mesh";
  return {};
}

// Imports the file a Model.path names and resolves its selection. Empty on
// success, with *scene holding everything the file ships and *index the mesh to
// take (-1 = every instance the file places).
//
// Deliberately not routed through AssetDatabase::LoadMesh: that converter takes
// bytes and returns ONE mesh, while a glTF file needs its own path to resolve
// external buffers and images and yields N meshes, N materials, N textures and
// the node instances that place them. The assets go into the database through
// the Add* side channel below, which is what it is for.
std::string ImportModel(const std::string& path, asset::ImportedScene* scene, i32* index) {
  std::string file;
  if (std::string problem = SplitModelPath(path, &file, index); !problem.empty()) return problem;
  if (!asset::LoadGltfScene(file, scene)) {
    return "does not import (the path is relative to the working directory)";
  }
  return SelectionProblem(*index, scene->meshes.size(), scene->instances.size());
}

// The Name.value of an entity, or empty. What Located narrows a finding by and
// what an anchor message names a loop with.
std::string EntityName(ecs::World& world, ecs::Entity entity) {
  const scene::Name* name = world.Get<scene::Name>(entity);
  return name ? name->value : std::string();
}

// Puts a build failure on the line that authored it. The pass runs on the
// loaded world, which no longer remembers where any value was written, so the
// scene is re-read to find the `key` assignment carrying `value`, tokenized the
// way the loader does (trim, skip blank and #/; comments).
//
// `owner` is the Name.value of the entity that carries the assignment, which is
// what keeps the finding on the right block when several entities write the
// same one - two cells of the same grid, two instances of the same prefab. An
// assignment that is not there verbatim (an unreadable file, an edit since the
// load, an entity a prefab expanded into) falls back to the first line that
// matches at all, and then to no line, rather than pointing at the wrong one.
std::string Located(const std::string& scene_path, const std::string& owner, std::string_view key,
                    const std::string& value, const std::string& problem) {
  std::ifstream in(scene_path, std::ios::binary);
  std::string line;
  int line_no = 0;
  int anywhere = 0;
  bool inside = owner.empty();
  while (std::getline(in, line)) {
    ++line_no;
    const size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos || line[a] == '#' || line[a] == ';') continue;
    const size_t b = line.find_last_not_of(" \t\r\n");
    const std::string_view text(line.data() + a, b - a + 1);
    if (!owner.empty()) {
      if (text == "entity") inside = false;
      if (text.starts_with("Name.value")) inside = text.find(owner) != std::string_view::npos;
    }
    if (!text.starts_with(key) || text.find(value) == std::string_view::npos) continue;
    if (inside) {
      return std::format("{}:{}: {} = \"{}\" {}", scene_path, line_no, key, value, problem);
    }
    if (anywhere == 0) anywhere = line_no;
  }
  if (anywhere != 0) {
    return std::format("{}:{}: {} = \"{}\" {}", scene_path, anywhere, key, value, problem);
  }
  return std::format("{}: {} = \"{}\" {}", scene_path, key, value, problem);
}

u64 PackKey(ecs::Entity entity) {
  return static_cast<u64>(entity.generation) << 32 | entity.index;
}

// File order. World::Create hands out ascending indices and LoadScene calls it
// once per `entity` block, so this is the order the author wrote, which is what
// decides which grid cell a member lands in.
void SortByDeclaration(std::vector<ecs::Entity>& entities) {
  std::sort(entities.begin(), entities.end(), [](ecs::Entity a, ecs::Entity b) {
    return a.index < b.index;
  });
}

// A prefab path is relative to the file that names it, not to the working
// directory (see ScenePrefab), so that a scene and the prefabs it instances
// move together.
std::string ResolveAgainst(const std::string& base, const std::string& path) {
  const std::filesystem::path relative(path);
  if (path.empty() || relative.is_absolute()) return path;
  const std::filesystem::path dir = std::filesystem::path(base).parent_path();
  if (dir.empty()) return path;
  return (dir / relative).lexically_normal().string();
}

// What an Anchor.target or a Grid.of resolves through. A name two entities
// share maps to kInvalidEntity rather than to whichever the walk reached last:
// placing half a scene against the wrong object is worse than refusing to.
class NameIndex {
 public:
  explicit NameIndex(ecs::World& world) {
    world.Each<scene::Name>([&](ecs::Entity entity, scene::Name& name) {
      if (name.value.empty()) return;
      auto [it, inserted] = by_name_.emplace(name.value, entity);
      if (!inserted) it->second = ecs::kInvalidEntity;
    });
  }

  // The entity called `name`, kInvalidEntity when no entity or two do. `*shared`
  // tells those apart, which is the difference between "you misspelt it" and
  // "you named it twice".
  ecs::Entity Find(const std::string& name, bool* shared) const {
    const auto it = by_name_.find(name);
    *shared = it != by_name_.end() && !it->second;
    return it == by_name_.end() ? ecs::kInvalidEntity : it->second;
  }

 private:
  std::unordered_map<std::string, ecs::Entity> by_name_;
};

// Why `name` names no single entity, or empty. Written once so a grid and an
// anchor explain a bad reference the same way.
std::string TargetProblem(const std::string& name, ecs::Entity resolved, bool shared) {
  if (name.empty()) return "is empty; a reference needs the target's Name.value";
  if (shared) return "names two entities; one Name has to mean one entity for a reference to work";
  if (!resolved) return "names no entity in this scene (references are by Name.value)";
  return {};
}

}  // namespace

u32 ShapeRequiredSizeAxes(std::string_view kind) {
  for (const ShapeKind& entry : kShapeKinds) {
    if (kind == entry.name) return entry.required_size_axes;
  }
  return 0;
}

void RegisterSceneComponents() {
  edit::ReflectComponent<SceneShape>("Shape")
      .Prop("kind", &SceneShape::kind)
      .Hint("box | sphere | plane | cylinder | cone | torus | capsule")
      .Prop("size", &SceneShape::size)
      .Hint("box, plane: half extents x y z; sphere: radius in x; cylinder, cone, capsule: "
            "radius in x, half height in y (a capsule adds a radius-tall cap at each end); "
            "torus: ring radius in x, tube radius in y");
  edit::ReflectComponent<SceneSurface>("Surface")
      .Prop("base_color", &SceneSurface::base_color)
      .Range(0.0f, 1.0f)
      .Prop("roughness", &SceneSurface::roughness)
      .Range(0.0f, 1.0f)
      .Prop("metallic", &SceneSurface::metallic)
      .Range(0.0f, 1.0f)
      .Prop("emissive", &SceneSurface::emissive)
      .Prop("clearcoat", &SceneSurface::clearcoat)
      .Range(0.0f, 1.0f)
      .Hint("clear lacquer over the base layer (car paint, wet stone); needs the sun")
      .Prop("clearcoat_roughness", &SceneSurface::clearcoat_roughness)
      .Range(0.0f, 1.0f)
      .Prop("anisotropy", &SceneSurface::anisotropy)
      .Range(-1.0f, 1.0f)
      .Hint("stretches the highlight along the surface tangent (brushed metal); needs the sun")
      .Prop("ior", &SceneSurface::ior)
      .Range(1.0f, 3.0f)
      .Hint("dielectric index of refraction, which sets a non-metal's specular level")
      .Prop("sheen_color", &SceneSurface::sheen_color)
      .Range(0.0f, 1.0f)
      .Hint("grazing-angle fuzz (velvet, felt); needs the sun")
      .Prop("sheen_roughness", &SceneSurface::sheen_roughness)
      .Range(0.0f, 1.0f)
      .Prop("subsurface_color", &SceneSurface::subsurface_color)
      .Range(0.0f, 1.0f)
      .Hint("also tints soft_lighting, rim_lighting and back_lighting")
      .Prop("subsurface", &SceneSurface::subsurface)
      .Range(0.0f, 1.0f)
      .Hint("wrap lighting + back scatter (wax, leaves, skin); needs the sun")
      .Prop("iridescence", &SceneSurface::iridescence)
      .Range(0.0f, 1.0f)
      .Hint("thin-film rainbow on the specular (soap, oil, beetle shell)")
      .Prop("iridescence_thickness", &SceneSurface::iridescence_thickness)
      .Range(100.0f, 1200.0f)
      .Hint("film thickness in nm; picks which colours the interference lands on")
      .Prop("transmission", &SceneSurface::transmission)
      .Range(0.0f, 1.0f)
      .Hint("refract the scene behind instead of diffusing; moves the shape to the "
            "transparent pass")
      .Prop("specular_color", &SceneSurface::specular_color)
      .Range(0.0f, 1.0f)
      .Prop("specular_strength", &SceneSurface::specular_strength)
      .Range(0.0f, 1.0f)
      .Hint("0 is matte however low the roughness")
      .Prop("env_reflect", &SceneSurface::env_reflect)
      .Range(0.0f, 1.0f)
      .Hint("fresnel-weighted reflection of the environment, layered over the base material")
      .Prop("soft_lighting", &SceneSurface::soft_lighting)
      .Range(0.0f, 1.0f)
      .Hint("spills the key light past the terminator; needs the sun")
      .Prop("rim_lighting", &SceneSurface::rim_lighting)
      .Range(0.0f, 8.0f)
      .Hint("backlit edge glow, the value being its falloff exponent; needs the sun")
      .Prop("back_lighting", &SceneSurface::back_lighting)
      .Range(0.0f, 1.0f)
      .Hint("light transmitted straight through; needs the sun")
      .Prop("materialx", &SceneSurface::materialx)
      .Hint("path to a .mtlx document to take the whole material from, replacing the fields "
            "above; empty to author them here");
  edit::ReflectComponent<ScenePattern>("Pattern")
      .Prop("kind", &ScenePattern::kind)
      .Hint("checker | grid | brick | gradient | noise")
      .Prop("scale", &ScenePattern::scale)
      .Hint("cells across the shape's uv square (brick: courses)")
      .Prop("color_a", &ScenePattern::color_a)
      .Range(0.0f, 1.0f)
      .Hint("checker/grid line/mortar/ramp start; multiplies Surface.base_color")
      .Prop("color_b", &ScenePattern::color_b)
      .Range(0.0f, 1.0f)
      .Hint("the raised end: cell interior, brick face, ramp end")
      .Prop("line_width", &ScenePattern::line_width)
      .Range(0.0f, 0.9f)
      .Hint("grid line / brick mortar width as a fraction of one cell")
      .Prop("seed", &ScenePattern::seed)
      .Hint("noise lattice seed")
      .Prop("resolution", &ScenePattern::resolution)
      .Hint("generated map size in texels, clamped to 4..2048")
      .Prop("relief", &ScenePattern::relief)
      .Range(0.0f, 0.5f)
      .Hint("depth of the generated normal map's relief in uv units; 0 binds no normal map")
      .Prop("roughness_a", &ScenePattern::roughness_a)
      .Range(0.0f, 1.0f)
      .Hint("roughness at the pattern's low end, multiplying Surface.roughness")
      .Prop("roughness_b", &ScenePattern::roughness_b)
      .Range(0.0f, 1.0f)
      .Hint("roughness at the high end; equal to roughness_a binds no map");
  edit::ReflectComponent<SceneModel>("Model")
      .Prop("path", &SceneModel::path)
      .Hint("a .gltf/.glb file to place whole (one child entity per instance it describes), or "
            "\"<file>#mesh<N>\" to place just that mesh of it at this entity's Transform; "
            "relative to the working directory");
  edit::ReflectComponent<ScenePrefab>("Prefab")
      .Prop("path", &ScenePrefab::path)
      .Hint("a .rxscene to instance here, relative to the file naming it; its first entity's "
            "components land on this entity (whatever this entity already says wins) and the "
            "rest become children of it");
  edit::ReflectComponent<SceneAnchor>("Anchor")
      .Prop("target", &SceneAnchor::target)
      .Hint("Name.value of the entity to stand against; this replaces Transform.position, and "
            "centres on the target across the two axes the mode does not stack along")
      .Prop("mode", &SceneAnchor::mode)
      .Hint("on | under | right | left | front | behind, measured from both entities' built "
            "geometry (front is +z, right is +x)");
  edit::ReflectComponent<SceneGrid>("Grid")
      .Prop("of", &SceneGrid::of)
      .Hint("Name.value of the grid this entity is a cell of; members fill the cells in the "
            "order the file declares them, and this replaces Transform.position")
      .Prop("cell", &SceneGrid::cell)
      .Hint("a prefab every member that does not instance one itself is an instance of; same "
            "resolution as Prefab.path")
      .Prop("count", &SceneGrid::count)
      .Hint("cells along x, y and z; this entity's Transform is cell 0 0 0")
      .Prop("step", &SceneGrid::step)
      .Hint("spacing between cells along each axis");
  edit::ReflectComponent<SceneLight>("Light")
      .Prop("color", &SceneLight::color)
      .Range(0.0f, 1.0f)
      .Prop("intensity", &SceneLight::intensity)
      .Prop("radius", &SceneLight::radius)
      .Hint("influence cutoff in meters");
  edit::ReflectComponent<SceneCamera>("Camera")
      .Prop("target", &SceneCamera::target)
      .Hint("world point the eye looks at")
      .Prop("fov_degrees", &SceneCamera::fov_degrees)
      .Range(10.0f, 150.0f);
}

bool BuildSceneShapes(ecs::World& world, asset::AssetDatabase& db, render::Renderer* renderer,
                      std::string* error) {
  // The Renderable is added after the walk: adding a component moves the entity
  // between archetypes, which must not happen under Each.
  std::vector<std::pair<ecs::Entity, asset::AssetId>> renderables;
  std::unordered_set<u64> built;
  // Keyed by mesh, not by entity: shapes that agree on every field share one
  // mesh, so the second entity onto a mesh never reaches the builder below.
  std::unordered_map<u64, SceneBounds> bounds;
  bool ok = true;

  world.Each<SceneShape>([&](ecs::Entity e, SceneShape& shape) {
    if (!ok) return;
    static const SceneSurface kDefaultSurface;
    const SceneSurface* found = world.Get<SceneSurface>(e);
    const SceneSurface& surface = found ? *found : kDefaultSurface;
    const ScenePattern* pattern = world.Get<ScenePattern>(e);

    const std::string key = ShapeKey(shape, surface, pattern);
    const asset::AssetId mesh_id = asset::MakeAssetId(key);
    renderables.emplace_back(e, mesh_id);
    if (!built.insert(mesh_id.hash).second) return;

    asset::Material material;
    if (surface.materialx.empty()) {
      ApplySurface(surface, &material);
    } else if (!asset::LoadMaterialX(surface.materialx, &material)) {
      if (error) *error = "Surface.materialx '" + surface.materialx + "' did not load";
      ok = false;
      return;
    }
    material.id = asset::MakeAssetId(key + "/material");
    if (pattern && !ApplyPattern(*pattern, key, db, renderer, &material, error)) {
      ok = false;
      return;
    }

    asset::Mesh mesh;
    if (!BuildShapeMesh(shape, mesh_id, &mesh, error)) {
      ok = false;
      return;
    }
    if (SceneBounds box; MeshBounds(mesh, &box)) bounds.emplace(mesh_id.hash, box);
    // MakeBox leaves the submesh list empty and the others append one blank;
    // either way the whole index range draws with this material.
    for (asset::MeshLod& lod : mesh.lods) {
      if (lod.submeshes.empty()) {
        lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material.id});
      } else {
        for (asset::Submesh& submesh : lod.submeshes) submesh.material = material.id;
      }
    }
    if (renderer) {
      renderer->UploadMaterial(material);
      renderer->UploadMesh(mesh);
    }
  });

  if (!ok) return false;
  for (const auto& [entity, mesh] : renderables) {
    // The frame walk is Each<Transform, Renderable>, so a shape authored
    // without a Transform (a ground plane at the origin needs no other field)
    // would upload fine and then never draw.
    if (!world.Has<scene::Transform>(entity)) world.Add(entity, scene::Transform{});
    world.Add(entity, scene::Renderable{mesh});
    if (const auto it = bounds.find(mesh.hash); it != bounds.end()) {
      world.Add(entity, it->second);
    }
  }
  return true;
}

std::string SceneModelProblem(const std::string& path) {
  asset::ImportedScene discarded;
  i32 index = -1;
  return ImportModel(path, &discarded, &index);
}

bool BuildSceneModels(ecs::World& world, asset::AssetDatabase& db, render::Renderer* renderer,
                      const std::string& scene_path, std::string* error) {
  // What one imported file leaves behind, so a second entity naming the same
  // file places it again without re-parsing it and, more importantly, without
  // uploading the same mesh ids to the gpu twice.
  struct Imported {
    std::vector<asset::AssetId> meshes;  // by source index, which is what #mesh<N> counts
    std::vector<SceneBounds> bounds;     // parallel to meshes; empty entries stay unmeasurable
    std::vector<bool> measured;
    std::vector<asset::ImportedScene::Instance> instances;
  };
  std::unordered_map<std::string, Imported> by_file;

  // Both are deferred out of the walk: adding a component moves the entity
  // between archetypes and creating one appends to them, neither of which may
  // happen under Each.
  struct Placed {
    ecs::Entity entity;  // the parent, for a Child
    asset::AssetId mesh;
    SceneBounds bounds;
    bool measured = false;
  };
  std::vector<Placed> renderables;
  struct Child : Placed {
    asset::ImportedScene::Instance placement;
  };
  std::vector<Child> children;
  bool ok = true;

  world.Each<SceneModel>([&](ecs::Entity e, SceneModel& model) {
    if (!ok) return;
    std::string file;
    i32 index = -1;
    std::string problem = SplitModelPath(model.path, &file, &index);
    auto imported = by_file.find(file);
    if (problem.empty() && imported == by_file.end()) {
      asset::ImportedScene scene;
      problem = ImportModel(model.path, &scene, &index);
      if (problem.empty()) {
        Imported& entry = by_file[file];
        // Order matters, exactly as in ApplyPattern: MaterialSystem resolves a
        // material's texture slots at UploadMaterial and silently falls back to
        // its 1x1 defaults for anything not uploaded yet, so every map has to
        // reach the gpu before the material naming it, and every material
        // before the mesh whose submeshes name it.
        for (asset::Texture& texture : scene.textures) {
          if (!texture.id) continue;  // a decode the importer already warned about
          if (renderer) renderer->UploadTexture(texture);
          db.AddTexture(std::move(texture));
        }
        for (const asset::Material& material : scene.materials) {
          if (renderer) renderer->UploadMaterial(material);
          db.AddMaterial(material);
        }
        for (asset::Mesh& mesh : scene.meshes) {
          if (renderer) renderer->UploadMesh(mesh);
          entry.meshes.push_back(mesh.id);
          SceneBounds box;
          entry.measured.push_back(MeshBounds(mesh, &box));
          entry.bounds.push_back(box);
          db.AddMesh(std::move(mesh));
        }
        entry.instances.assign(scene.instances.begin(), scene.instances.end());
        imported = by_file.find(file);
      }
    } else if (problem.empty()) {
      // The file is known good; only this entity's own selection is not.
      problem = SelectionProblem(index, imported->second.meshes.size(),
                                 imported->second.instances.size());
    }
    if (!problem.empty()) {
      if (error)
        *error = Located(scene_path, EntityName(world, e), "Model.path", model.path, problem);
      ok = false;
      return;
    }

    const Imported& file_assets = imported->second;
    auto placed = [&](ecs::Entity entity, size_t mesh_index) {
      return Placed{entity, file_assets.meshes[mesh_index], file_assets.bounds[mesh_index],
                    file_assets.measured[mesh_index]};
    };
    if (index >= 0) {
      renderables.push_back(placed(e, static_cast<size_t>(index)));
      return;
    }
    for (const asset::ImportedScene::Instance& instance : file_assets.instances) {
      children.push_back({placed(e, instance.mesh_index), instance});
    }
  });

  if (!ok) return false;
  for (const Placed& placed : renderables) {
    // Same reason as BuildSceneShapes: the frame walk is Each<Transform,
    // Renderable>, so a model authored without a Transform would upload and
    // then never draw.
    if (!world.Has<scene::Transform>(placed.entity)) {
      world.Add(placed.entity, scene::Transform{});
    }
    world.Add(placed.entity, scene::Renderable{placed.mesh});
    if (placed.measured) world.Add(placed.entity, placed.bounds);
  }
  for (const Child& child : children) {
    // The file's own layout stays in the children and composes through Parent,
    // so the authored Transform moves the whole model and nothing here has to
    // multiply two TRS by hand. Transient because a SaveScene has to write the
    // Model that placed them and not the entities it expanded into, or the next
    // load would place the file twice.
    ecs::Entity entity = world.Create();
    scene::Transform transform;
    transform.position[0] = child.placement.position.x;
    transform.position[1] = child.placement.position.y;
    transform.position[2] = child.placement.position.z;
    std::memcpy(transform.rotation, child.placement.rotation, sizeof(transform.rotation));
    transform.scale = child.placement.scale;
    world.Add(entity, transform);
    world.Add(entity, scene::Renderable{child.mesh});
    world.Add(entity, scene::Parent{child.entity});
    world.Add(entity, scene::Transient{});
    // On the child rather than on the authored entity: BuildSceneAnchors unions
    // a subtree, so a model anchored onto something measures every instance the
    // file placed, at the offsets the file placed them.
    if (child.measured) world.Add(entity, child.bounds);
  }
  return true;
}

namespace {

// One prefab file, loaded once however many entities instance it. The loaded
// world is what is kept, rather than the text: expansion copies components, and
// the loader is the only thing that turns one into the other.
//
// INVARIANT, the same one scene_validate.cc rests on: the world is freshly
// constructed and nothing but LoadScene touches it, so World::Create hands out
// indices 0, 1, 2, ... in `entity` block order. That is what makes "the file's
// first entity" a thing this code can point at.
struct PrefabFile {
  asset::Vfs vfs;
  asset::AssetDatabase db{vfs};
  ecs::World world;
  std::vector<ecs::Entity> entities;  // file order; entities[0] is the prefab root
};

// Loads a prefab file, or the clause saying why it is not instanceable. Strict,
// like every other .rxscene the runtime loads: a prefab that silently dropped a
// misspelt component would place a subtly wrong thing everywhere it is used.
std::string OpenPrefab(const std::string& resolved, PrefabFile* file) {
  std::string error;
  if (!edit::LoadScene(file->world, file->db, resolved, &error, /*strict=*/true)) {
    return std::format("does not load as '{}': {}", resolved, error);
  }
  const size_t count = file->world.entity_count();
  for (size_t index = 0; index < count; ++index) {
    const ecs::Entity entity{static_cast<u32>(index), 0};
    if (file->world.IsAlive(entity)) file->entities.push_back(entity);
  }
  if (file->entities.empty()) return "declares no entity; there is nothing to instance";
  return {};
}

// Copies `from`'s reflected components onto `to`. `keep_existing` is the prefab
// override rule: a component the instance already authored is left alone, which
// is what lets one prefab serve many variants. Entity-valued props go through
// `remap`, so a Parent inside the prefab points at this copy of the prefab and
// not at the file's own entity.
//
// Guid is never copied: two entities sharing one make every reference to it
// ambiguous and make a re-save drop one of them. SceneBounds is not reflected,
// so it cannot be copied either, which is correct - the copy's bounds come from
// the geometry the copy builds.
void CopyComponents(ecs::World& src, ecs::Entity from, ecs::World& dst, ecs::Entity to,
                    bool keep_existing, const std::unordered_map<u64, ecs::Entity>& remap) {
  for (const edit::ComponentDesc* comp : edit::ComponentsOn(src, from)) {
    if (comp->id == ecs::GetComponentId<scene::Guid>()) continue;
    if (dst.HasRaw(to, comp->id)) {
      if (keep_existing) continue;
    } else {
      edit::AddComponentByDesc(dst, to, *comp);
    }
    for (u32 i = 0; i < comp->prop_count; ++i) {
      const edit::PropDesc& prop = comp->props[i];
      edit::PropValue value;
      if (!edit::GetProp(src, from, *comp, prop, &value)) continue;
      if (prop.type == edit::PropType::kEntity) {
        const auto found = remap.find(PackKey(value.e));
        value = edit::PropValue::EntityV(found == remap.end() ? ecs::kInvalidEntity
                                                             : found->second);
      }
      edit::SetProp(dst, to, *comp, prop, value);
    }
  }
}

// The axis a mode stacks along and which way round the two boxes meet: true
// puts the placed entity's low face on the target's high face. The two axes a
// mode does not name centre, which is what makes "on" put a ball in the middle
// of a crate rather than over its corner.
struct AnchorMode {
  const char* name;
  u32 axis;
  bool ascending;
};
constexpr AnchorMode kAnchorModes[] = {
    {"on", 1, true},     {"under", 1, false}, {"right", 0, true},
    {"left", 0, false},  {"front", 2, true},  {"behind", 2, false},
};

const AnchorMode* FindAnchorMode(const std::string& name) {
  for (const AnchorMode& mode : kAnchorModes) {
    if (name == mode.name) return &mode;
  }
  return nullptr;
}

struct Aabb {
  f32 min[3] = {0, 0, 0};
  f32 max[3] = {0, 0, 0};
  bool empty = true;
};

void Include(Aabb* box, const Vec3& point) {
  const f32 p[3] = {point.x, point.y, point.z};
  for (u32 axis = 0; axis < 3; ++axis) {
    box->min[axis] = box->empty ? p[axis] : std::min(box->min[axis], p[axis]);
    box->max[axis] = box->empty ? p[axis] : std::max(box->max[axis], p[axis]);
  }
  box->empty = false;
}

using ChildMap = std::unordered_map<u64, std::vector<ecs::Entity>>;

ChildMap MapChildren(ecs::World& world) {
  ChildMap children;
  world.Each<scene::Parent>([&](ecs::Entity entity, scene::Parent& parent) {
    if (parent.value) children[PackKey(parent.value)].push_back(entity);
  });
  return children;
}

// The bounds of `entity` and everything parented under it, as placed by `at`.
// The subtree matters: a Model keeps its geometry in the children
// BuildSceneModels expanded it into, and a prefab instance in the children
// BuildScenePrefabs did, so measuring only the entity itself would measure
// nothing at all for both. All eight corners go through the transform, not just
// min and max: under a rotation those two corners are no longer the extremes.
void AccumulateBounds(ecs::World& world, const ChildMap& children, ecs::Entity entity,
                      const scene::Transform& at, Aabb* out, int depth) {
  // A Parent cycle (which --validate reports and a load does not refuse) would
  // otherwise never return.
  if (depth > 64) return;
  if (const SceneBounds* box = world.Get<SceneBounds>(entity)) {
    const Quat rotation{at.rotation[0], at.rotation[1], at.rotation[2], at.rotation[3]};
    const Vec3 origin{at.position[0], at.position[1], at.position[2]};
    for (u32 corner = 0; corner < 8; ++corner) {
      const Vec3 local{corner & 1 ? box->max[0] : box->min[0],
                       corner & 2 ? box->max[1] : box->min[1],
                       corner & 4 ? box->max[2] : box->min[2]};
      Include(out, origin + Rotate(rotation, local * at.scale));
    }
  }
  const auto found = children.find(PackKey(entity));
  if (found == children.end()) return;
  for (ecs::Entity child : found->second) {
    const scene::Transform* local = world.Get<scene::Transform>(child);
    AccumulateBounds(world, children, child,
                     edit::ComposeTransform(at, local ? *local : scene::Transform{}), out,
                     depth + 1);
  }
}

Aabb SubtreeBounds(ecs::World& world, const ChildMap& children, ecs::Entity entity,
                   const scene::Transform& at) {
  Aabb box;
  AccumulateBounds(world, children, entity, at, &box, 0);
  return box;
}

// Orders the anchored entities so a placement never reads a position that has
// not settled. A depth-first walk over the two links a position can depend on -
// the entity an Anchor names, and the Parent that moves it - is the whole of
// the dependency handling; an anchor names exactly one target, so there is
// nothing here to relax or iterate.
//
// `state` is 0 unseen, 1 on the stack, 2 settled. Reaching a 1 is the cycle the
// load has to refuse, and the stack at that moment is the loop to name.
class AnchorOrder {
 public:
  AnchorOrder(ecs::World& world, const NameIndex& names) : world_(world), names_(names) {}

  bool Visit(ecs::Entity entity) {
    const auto seen = state_.find(PackKey(entity));
    if (seen != state_.end()) {
      if (seen->second == 2) return true;
      ReportCycle(entity);
      return false;
    }
    state_.emplace(PackKey(entity), 1);
    stack_.push_back(entity);

    if (const SceneAnchor* anchor = world_.Get<SceneAnchor>(entity)) {
      const std::string target = anchor->target;
      bool shared = false;
      const ecs::Entity to = names_.Find(target, &shared);
      if (std::string why = TargetProblem(target, to, shared); !why.empty()) {
        owner_ = EntityName(world_, entity);
        offender_ = target;
        problem_ = std::move(why);
        return false;
      }
      if (!Visit(to)) return false;
    }
    // A parent carries its children, so an anchor onto a grid member has to
    // wait for whatever settles the grid itself.
    if (const scene::Parent* parent = world_.Get<scene::Parent>(entity)) {
      if (parent->value && world_.IsAlive(parent->value) && !Visit(parent->value)) return false;
    }

    stack_.pop_back();
    state_[PackKey(entity)] = 2;
    if (world_.Has<SceneAnchor>(entity)) order_.push_back(entity);
    return true;
  }

  const std::vector<ecs::Entity>& order() const { return order_; }
  // The entity carrying the assignment that failed, and the value it named.
  const std::string& owner() const { return owner_; }
  const std::string& offender() const { return offender_; }
  const std::string& problem() const { return problem_; }

 private:
  void ReportCycle(ecs::Entity closed) {
    if (!problem_.empty()) return;  // the first failure is the one worth naming
    std::string loop;
    bool inside = false;
    for (ecs::Entity step : stack_) {
      inside = inside || step == closed;
      if (!inside) continue;
      const std::string step_name = EntityName(world_, step);
      loop += (step_name.empty() ? "<unnamed>" : step_name) + " -> ";
    }
    // The link that closed the loop is the one on top of the stack, so that is
    // the Anchor.target line to put the finding on.
    owner_ = stack_.empty() ? std::string() : EntityName(world_, stack_.back());
    offender_ = EntityName(world_, closed);
    problem_ = std::format("closes a cycle ({}{}); anchors resolve in dependency order, which a "
                           "loop has none of", loop, offender_);
  }

  ecs::World& world_;
  const NameIndex& names_;
  std::unordered_map<u64, int> state_;
  std::vector<ecs::Entity> stack_;
  std::vector<ecs::Entity> order_;
  std::string owner_;
  std::string offender_;
  std::string problem_;
};

}  // namespace

bool BuildSceneGrids(ecs::World& world, const std::string& scene_path, std::string* error) {
  std::vector<ecs::Entity> members;
  world.Each<SceneGrid>([&](ecs::Entity entity, SceneGrid& grid) {
    if (!grid.of.empty()) members.push_back(entity);
  });
  if (members.empty()) return true;
  SortByDeclaration(members);

  const NameIndex names(world);
  std::unordered_map<u64, u32> taken;  // container -> cells already claimed
  // Deferred out of the loop below for the usual reason: adding Parent or
  // Transform moves an entity between archetypes and invalidates every pointer
  // into the storage the loop is reading.
  struct Placement {
    ecs::Entity entity;
    ecs::Entity container;
    f32 offset[3];
    std::string cell;
  };
  std::vector<Placement> placements;

  for (ecs::Entity entity : members) {
    const std::string of = world.Get<SceneGrid>(entity)->of;
    const std::string owner = EntityName(world, entity);
    bool shared = false;
    const ecs::Entity container = names.Find(of, &shared);
    if (std::string why = TargetProblem(of, container, shared); !why.empty()) {
      if (error) *error = Located(scene_path, owner, "Grid.of", of, why);
      return false;
    }
    if (container == entity) {
      if (error)
        *error = Located(scene_path, owner, "Grid.of", of,
                         "is this entity; a grid cannot be a cell of itself");
      return false;
    }

    const SceneGrid& grid = *world.Get<SceneGrid>(container);
    u32 extent[3] = {0, 0, 0};
    for (u32 axis = 0; axis < 3; ++axis) {
      extent[axis] = grid.count[axis] >= 1.0f ? static_cast<u32>(grid.count[axis]) : 0;
    }
    if (extent[0] == 0 || extent[1] == 0 || extent[2] == 0) {
      if (error)
        *error = Located(scene_path, owner, "Grid.of", of,
                         std::format("names a grid whose Grid.count is {} {} {}; an axis with no "
                                     "cells leaves it nowhere to put anything",
                                     grid.count[0], grid.count[1], grid.count[2]));
      return false;
    }

    const u32 cell = taken[PackKey(container)]++;
    const u32 cells = extent[0] * extent[1] * extent[2];
    if (cell >= cells) {
      if (error)
        *error = Located(scene_path, owner, "Grid.of", of,
                         std::format("names a grid of {} cells and this is its member {}; two "
                                     "members on one coordinate read as a missing object",
                                     cells, cell + 1));
      return false;
    }
    Placement placement{entity, container, {}, grid.cell};
    const u32 index[3] = {cell % extent[0], (cell / extent[0]) % extent[1],
                          cell / (extent[0] * extent[1])};
    for (u32 axis = 0; axis < 3; ++axis) {
      placement.offset[axis] = static_cast<f32>(index[axis]) * grid.step[axis];
    }
    placements.push_back(std::move(placement));
  }

  for (const Placement& placement : placements) {
    // Replaced, not added to: a SaveScene writes the cell a member was given
    // alongside the Grid.of that gave it, so a layout that added would walk the
    // member one cell further along on every save/load round trip.
    if (scene::Transform* authored = world.Get<scene::Transform>(placement.entity)) {
      std::memcpy(authored->position, placement.offset, sizeof(authored->position));
    } else {
      scene::Transform cell;
      std::memcpy(cell.position, placement.offset, sizeof(cell.position));
      world.Add(placement.entity, cell);
    }
    // Parented rather than resolved to a world position, so the container's
    // Transform still moves the whole grid after the layout has run, and so a
    // member's saved position is the cell offset it can be given again rather
    // than a world coordinate baked out of the container.
    if (scene::Parent* parent = world.Get<scene::Parent>(placement.entity)) {
      parent->value = placement.container;
    } else {
      world.Add(placement.entity, scene::Parent{placement.container});
    }
    if (!placement.cell.empty() && !world.Has<ScenePrefab>(placement.entity)) {
      world.Add(placement.entity, ScenePrefab{placement.cell});
    }
  }
  return true;
}

bool BuildScenePrefabs(ecs::World& world, const std::string& scene_path, std::string* error) {
  std::unordered_map<std::string, std::unique_ptr<PrefabFile>> files;
  // The file an entity came from, so a prefab instancing another resolves it
  // relative to itself, and the chain of files that produced it, so a prefab
  // reaching itself is caught on the second visit instead of by expanding until
  // memory runs out.
  std::unordered_map<u64, std::string> origin;
  std::unordered_map<u64, std::vector<std::string>> chain;
  std::unordered_set<u64> expanded;

  // A pass at a time, because expanding one prefab can introduce another: a
  // prefab file whose own entities instance further prefabs.
  for (;;) {
    std::vector<ecs::Entity> pending;
    world.Each<ScenePrefab>([&](ecs::Entity entity, ScenePrefab&) {
      if (!expanded.contains(PackKey(entity))) pending.push_back(entity);
    });
    if (pending.empty()) return true;
    SortByDeclaration(pending);

    for (ecs::Entity entity : pending) {
      const u64 key = PackKey(entity);
      expanded.insert(key);
      // Copied out before anything is added to the entity: an added component
      // moves it between archetypes and leaves the pointer dangling.
      const std::string path = world.Get<ScenePrefab>(entity)->path;
      const auto from = origin.find(key);
      const std::string base = from == origin.end() ? scene_path : from->second;
      const std::string resolved = ResolveAgainst(base, path);
      const std::vector<std::string> reached = chain[key];
      const std::string owner = EntityName(world, entity);

      if (std::find(reached.begin(), reached.end(), resolved) != reached.end()) {
        if (error) {
          std::string loop;
          for (const std::string& step : reached) loop += step + " -> ";
          *error = Located(base, owner, "Prefab.path", path,
                           std::format("instances itself ({}{}); the expansion would not "
                                       "terminate", loop, resolved));
        }
        return false;
      }

      auto file = files.find(resolved);
      if (file == files.end()) {
        auto opened = std::make_unique<PrefabFile>();
        if (std::string why = OpenPrefab(resolved, opened.get()); !why.empty()) {
          if (error) *error = Located(base, owner, "Prefab.path", path, why);
          return false;
        }
        file = files.emplace(resolved, std::move(opened)).first;
      }
      PrefabFile& prefab = *file->second;

      // Every copy exists before anything is copied into it, so a Parent inside
      // the prefab has a destination entity to be remapped onto. The root maps
      // onto the instance itself, which is what turns "Parent = the root" in the
      // file into "Parent = this instance" here.
      std::unordered_map<u64, ecs::Entity> remap;
      remap.emplace(PackKey(prefab.entities[0]), entity);
      std::vector<ecs::Entity> copies;
      for (size_t i = 1; i < prefab.entities.size(); ++i) {
        copies.push_back(world.Create());
        remap.emplace(PackKey(prefab.entities[i]), copies.back());
      }

      CopyComponents(prefab.world, prefab.entities[0], world, entity, /*keep_existing=*/true,
                     remap);
      std::vector<std::string> descend = reached;
      descend.push_back(resolved);
      for (size_t i = 1; i < prefab.entities.size(); ++i) {
        const ecs::Entity copy = copies[i - 1];
        CopyComponents(prefab.world, prefab.entities[i], world, copy, /*keep_existing=*/false,
                       remap);
        // Parented to the instance (unless the prefab parents it inside itself)
        // so the authored Transform moves the whole group, and Transient so a
        // SaveScene writes the Prefab.path that placed them rather than the
        // entities it expanded into - which on the next load would place the
        // file twice. Exactly what BuildSceneModels does with a glTF's nodes.
        if (!world.Has<scene::Parent>(copy)) world.Add(copy, scene::Parent{entity});
        world.Add(copy, scene::Transient{});
        origin.emplace(PackKey(copy), resolved);
        chain.emplace(PackKey(copy), descend);
      }
    }
  }
}

bool BuildSceneAnchors(ecs::World& world, const std::string& scene_path, std::string* error) {
  std::vector<ecs::Entity> anchored;
  world.Each<SceneAnchor>([&](ecs::Entity entity, SceneAnchor&) { anchored.push_back(entity); });
  if (anchored.empty()) return true;
  SortByDeclaration(anchored);

  const NameIndex names(world);
  AnchorOrder order(world, names);
  for (ecs::Entity entity : anchored) {
    if (order.Visit(entity)) continue;
    if (error)
      *error = Located(scene_path, order.owner(), "Anchor.target", order.offender(),
                       order.problem());
    return false;
  }

  const ChildMap children = MapChildren(world);
  for (ecs::Entity entity : order.order()) {
    const SceneAnchor& anchor = *world.Get<SceneAnchor>(entity);
    const std::string target_name = anchor.target;
    const std::string owner = EntityName(world, entity);
    const AnchorMode* mode = FindAnchorMode(anchor.mode);
    if (!mode) {
      if (error)
        *error = Located(scene_path, owner, "Anchor.mode", anchor.mode,
                         "is not a placement (on | under | right | left | front | behind)");
      return false;
    }
    // A grid cell and an anchor are two answers to the same question, and the
    // parent-relative one would silently win: the anchor writes a world
    // position that then composes through the container's transform.
    if (world.Has<scene::Parent>(entity)) {
      if (error)
        *error = Located(scene_path, owner, "Anchor.target", target_name,
                         "is an anchor on an entity that also has a parent (a Grid cell, or an "
                         "authored Parent); only one of the two can place it");
      return false;
    }

    bool shared = false;
    const ecs::Entity target = names.Find(target_name, &shared);
    const Aabb around = SubtreeBounds(world, children, target,
                                      edit::WorldTransform(world, target));
    if (around.empty) {
      if (error)
        *error = Located(scene_path, owner, "Anchor.target", target_name,
                         "names an entity with no built geometry, so there is no face to stand "
                         "against (only a Shape or a Model can be anchored onto)");
      return false;
    }

    scene::Transform local;
    if (const scene::Transform* authored = world.Get<scene::Transform>(entity)) local = *authored;
    // Measured about the entity's own origin, since its position is what this
    // is solving for. An entity with nothing to measure is left as the empty
    // box, whose min and max are both the origin, which places a Light or a
    // marker at the target's surface rather than refusing it - only the TARGET
    // has to have an extent for "on top of" to mean anything.
    scene::Transform upright = local;
    std::memset(upright.position, 0, sizeof(upright.position));
    const Aabb self = SubtreeBounds(world, children, entity, upright);

    // Replaced rather than offset: a SaveScene writes the resolved position
    // next to the Anchor that produced it, so anything additive here would
    // reload with the placement applied on top of itself.
    for (u32 axis = 0; axis < 3; ++axis) {
      if (axis == mode->axis) {
        local.position[axis] = mode->ascending ? around.max[axis] - self.min[axis]
                                               : around.min[axis] - self.max[axis];
      } else {
        local.position[axis] = 0.5f * (around.min[axis] + around.max[axis]) -
                               0.5f * (self.min[axis] + self.max[axis]);
      }
    }
    if (world.Has<scene::Transform>(entity)) {
      *world.Get<scene::Transform>(entity) = local;
    } else {
      world.Add(entity, local);
    }
  }
  return true;
}

}  // namespace rx
