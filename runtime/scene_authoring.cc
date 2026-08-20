#include "scene_authoring.h"

#include <algorithm>
#include <cstddef>
#include <unordered_set>
#include <utility>
#include <vector>

#include "asset/materialx.h"
#include "asset/primitives.h"
#include "asset/procedural_texture.h"
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

}  // namespace rx
