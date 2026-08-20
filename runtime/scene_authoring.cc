#include "scene_authoring.h"

#include <format>
#include <unordered_set>
#include <utility>
#include <vector>

#include "asset/primitives.h"
#include "edit/reflect.h"
#include "scene/components.h"

namespace rx {

void RegisterSceneComponents() {
  edit::ReflectComponent<SceneShape>("Shape")
      .Prop("kind", &SceneShape::kind)
      .Hint("box | sphere")
      .Prop("size", &SceneShape::size)
      .Hint("box: half extents x y z; sphere: radius in x");
  edit::ReflectComponent<SceneSurface>("Surface")
      .Prop("base_color", &SceneSurface::base_color)
      .Range(0.0f, 1.0f)
      .Prop("roughness", &SceneSurface::roughness)
      .Range(0.0f, 1.0f)
      .Prop("metallic", &SceneSurface::metallic)
      .Range(0.0f, 1.0f)
      .Prop("emissive", &SceneSurface::emissive);
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

bool BuildSceneShapes(ecs::World& world, render::Renderer* renderer, std::string* error) {
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

    // Every field that changes the built asset goes into the key, so two
    // entities share a mesh exactly when sharing one is correct.
    const std::string key = std::format(
        "rxscene/{}/{:g},{:g},{:g}/{:g},{:g},{:g}/{:g}/{:g}/{:g},{:g},{:g}", shape.kind,
        shape.size[0], shape.size[1], shape.size[2], surface.base_color[0],
        surface.base_color[1], surface.base_color[2], surface.roughness, surface.metallic,
        surface.emissive[0], surface.emissive[1], surface.emissive[2]);
    const asset::AssetId mesh_id = asset::MakeAssetId(key);
    renderables.emplace_back(e, mesh_id);
    if (!built.insert(mesh_id.hash).second) return;

    asset::Material material;
    material.id = asset::MakeAssetId(key + "/material");
    for (int i = 0; i < 3; ++i) {
      material.base_color_factor[i] = surface.base_color[i];
      material.emissive_factor[i] = surface.emissive[i];
    }
    material.roughness_factor = surface.roughness;
    material.metallic_factor = surface.metallic;

    asset::Mesh mesh;
    if (shape.kind == "box") {
      mesh = asset::MakeBox(shape.size[0], shape.size[1], shape.size[2], mesh_id);
    } else if (shape.kind == "sphere") {
      mesh = asset::MakeSphere(shape.size[0], 24, 32, mesh_id);
    } else {
      if (error) *error = "unknown Shape.kind '" + shape.kind + "' (box | sphere)";
      ok = false;
      return;
    }
    // MakeBox leaves the submesh list empty and MakeSphere appends one blank;
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
  for (const auto& [entity, mesh] : renderables) world.Add(entity, scene::Renderable{mesh});
  return true;
}

}  // namespace rx
