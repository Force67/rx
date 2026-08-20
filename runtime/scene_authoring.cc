#include "scene_authoring.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "asset/gltf_loader.h"
#include "asset/materialx.h"
#include "asset/primitives.h"
#include "asset/procedural_texture.h"
#include "asset/scene_import.h"
#include "edit/reflect.h"
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

// Puts a build failure on the line that authored it. The pass runs on the
// loaded world, which no longer remembers where any value was written, so the
// scene is re-read to find the assignment carrying `value`, tokenized the way
// the loader does (trim, skip blank and #/; comments). A value that is not
// there verbatim (an unreadable file, an edit since the load) reports with no
// line rather than pointing at the wrong one.
std::string Located(const std::string& scene_path, const std::string& value,
                    const std::string& problem) {
  std::ifstream in(scene_path, std::ios::binary);
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const size_t a = line.find_first_not_of(" \t\r\n");
    if (a == std::string::npos || line[a] == '#' || line[a] == ';') continue;
    if (line.compare(a, 10, "Model.path") != 0) continue;
    if (line.find(value, a) == std::string::npos) continue;
    return std::format("{}:{}: Model.path = \"{}\" {}", scene_path, line_no, value, problem);
  }
  return std::format("{}: Model.path = \"{}\" {}", scene_path, value, problem);
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
    std::vector<asset::ImportedScene::Instance> instances;
  };
  std::unordered_map<std::string, Imported> by_file;

  // Both are deferred out of the walk: adding a component moves the entity
  // between archetypes and creating one appends to them, neither of which may
  // happen under Each.
  std::vector<std::pair<ecs::Entity, asset::AssetId>> renderables;
  struct Child {
    ecs::Entity parent;
    asset::AssetId mesh;
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
      if (error) *error = Located(scene_path, model.path, problem);
      ok = false;
      return;
    }

    if (index >= 0) {
      renderables.emplace_back(e, imported->second.meshes[static_cast<size_t>(index)]);
      return;
    }
    for (const asset::ImportedScene::Instance& instance : imported->second.instances) {
      children.push_back({e, imported->second.meshes[instance.mesh_index], instance});
    }
  });

  if (!ok) return false;
  for (const auto& [entity, mesh] : renderables) {
    // Same reason as BuildSceneShapes: the frame walk is Each<Transform,
    // Renderable>, so a model authored without a Transform would upload and
    // then never draw.
    if (!world.Has<scene::Transform>(entity)) world.Add(entity, scene::Transform{});
    world.Add(entity, scene::Renderable{mesh});
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
    world.Add(entity, scene::Parent{child.parent});
    world.Add(entity, scene::Transient{});
  }
  return true;
}

}  // namespace rx
