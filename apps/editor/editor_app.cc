#include "editor_app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

#include "anim/morph.h"
#include "app/host.h"
#include "asset/blend_import.h"
#include "asset/gltf_loader.h"
#include "asset/usd_loader.h"
#include "asset/primitives.h"
#include "asset/vfs.h"
#include "core/log.h"
#include "edit/hierarchy.h"
#include "editor_input.h"
#include "render/core/settings.h"
#include "scene/components.h"

namespace rx::editor {
namespace fs = std::filesystem;

namespace {
Mat4 MatOf(const scene::Transform &t) {
  return MakeTransform(
      {t.position[0], t.position[1], t.position[2]},
      Quat{t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]},
      t.scale);
}
constexpr f32 kTopBar = 44.0f;
constexpr f32 kViewportTop = 74.0f; // topbar + viewport bar
constexpr f32 kLeftPanel = 300.0f;
constexpr f32 kRightPanel = 360.0f;
constexpr f32 kBottomPanels = 186.0f + 26.0f; // content + statusbar

asset::Mesh MakeTurntable(f32 radius, f32 half_height, asset::AssetId id,
                          asset::AssetId material) {
  constexpr u32 kSegments = 64;
  asset::Mesh mesh;
  mesh.id = id;
  mesh.lods.resize(1);
  asset::MeshLod &lod = mesh.lods[0];
  auto vertex = [](Vec3 position, Vec3 normal, f32 u, f32 v) {
    asset::Vertex out{};
    out.position[0] = position.x;
    out.position[1] = position.y;
    out.position[2] = position.z;
    out.normal[0] = normal.x;
    out.normal[1] = normal.y;
    out.normal[2] = normal.z;
    out.tangent[0] = 1;
    out.tangent[3] = 1;
    out.uv[0] = u;
    out.uv[1] = v;
    return out;
  };

  // Separate cap and rim vertices keep the top crisp while the outer wall is
  // smoothly shaded. A subtle bevel would cost more geometry without helping
  // this preview prop.
  const u32 top_center = static_cast<u32>(lod.vertices.size());
  lod.vertices.push_back(vertex({0, half_height, 0}, {0, 1, 0}, 0.5f, 0.5f));
  const u32 top_ring = static_cast<u32>(lod.vertices.size());
  for (u32 i = 0; i <= kSegments; ++i) {
    const f32 angle = static_cast<f32>(i) / kSegments * 6.28318530718f;
    const f32 x = std::cos(angle), z = std::sin(angle);
    lod.vertices.push_back(vertex({x * radius, half_height, z * radius},
                                  {0, 1, 0}, x * 0.5f + 0.5f,
                                  z * 0.5f + 0.5f));
  }
  const u32 bottom_center = static_cast<u32>(lod.vertices.size());
  lod.vertices.push_back(vertex({0, -half_height, 0}, {0, -1, 0}, 0.5f, 0.5f));
  const u32 bottom_ring = static_cast<u32>(lod.vertices.size());
  for (u32 i = 0; i <= kSegments; ++i) {
    const f32 angle = static_cast<f32>(i) / kSegments * 6.28318530718f;
    const f32 x = std::cos(angle), z = std::sin(angle);
    lod.vertices.push_back(vertex({x * radius, -half_height, z * radius},
                                  {0, -1, 0}, x * 0.5f + 0.5f,
                                  z * 0.5f + 0.5f));
  }
  const u32 side_ring = static_cast<u32>(lod.vertices.size());
  for (u32 i = 0; i <= kSegments; ++i) {
    const f32 angle = static_cast<f32>(i) / kSegments * 6.28318530718f;
    const f32 x = std::cos(angle), z = std::sin(angle);
    lod.vertices.push_back(
        vertex({x * radius, half_height, z * radius}, {x, 0, z},
               static_cast<f32>(i) / kSegments, 0));
    lod.vertices.push_back(
        vertex({x * radius, -half_height, z * radius}, {x, 0, z},
               static_cast<f32>(i) / kSegments, 1));
  }
  for (u32 i = 0; i < kSegments; ++i) {
    lod.indices.push_back(top_center);
    lod.indices.push_back(top_ring + i + 1);
    lod.indices.push_back(top_ring + i);
    lod.indices.push_back(bottom_center);
    lod.indices.push_back(bottom_ring + i);
    lod.indices.push_back(bottom_ring + i + 1);
    const u32 side = side_ring + i * 2;
    lod.indices.push_back(side);
    lod.indices.push_back(side + 2);
    lod.indices.push_back(side + 1);
    lod.indices.push_back(side + 1);
    lod.indices.push_back(side + 2);
    lod.indices.push_back(side + 3);
  }
  lod.submeshes.push_back(
      {0, static_cast<u32>(lod.indices.size()), material});
  mesh.bounds_radius = std::sqrt(radius * radius + half_height * half_height);
  return mesh;
}
} // namespace

// ===========================================================================
// Lifecycle
// ===========================================================================
bool Editor::OnInitialize(app::Services &s) {
  services_ = &s;
  host_ = s.host;
  window_ = s.window;
  renderer_ = s.renderer;
  world_ = s.world;
  input_map_ = s.input_map;
  actions_ = s.actions;
  vfs_ = s.vfs;
  headless_ = (window_ == nullptr);

  RegisterEditorInput(*input_map_);

  if (vfs_ && fs::exists(asset_root_))
    vfs_->Mount(asset::MakeLooseFileProvider(asset_root_));
  if (vfs_)
    assets_.emplace(*vfs_);

  if (renderer_) {
    renderer_->settings().ambient = 0.16f; // lift shadows a touch for editing
  }

  SetupDefaultScene();

  camera_.set_position({6.0f, 4.5f, 6.0f});
  Vec3 d = Normalize(Vec3{0, 1.0f, 0} - camera_.position());
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z),
                        std::asin(std::clamp(d.y, -1.0f, 1.0f)));

  // Load a scene or authoring model passed on argv.
  if (!open_path_.empty()) {
    OpenDocument(open_path_);
  }

  ScanAssets();

  if (!headless_) {
    if (!UiInit())
      RX_WARN("editor: ugui init failed; running without UI");
  }
  return true;
}

void Editor::OnShutdown() {
  if (ui_ready_)
    UiShutdown();
}

// ===========================================================================
// Scene / assets
// ===========================================================================
void Editor::SetupDefaultScene() {
  // One neutral material; per-entity variety comes from DrawItem tint.
  asset::Material mat;
  mat.id = asset::MakeAssetId("editor/default_mat");
  mat.base_color_factor[0] = mat.base_color_factor[1] =
      mat.base_color_factor[2] = 1.0f;
  mat.metallic_factor = 0.0f;
  mat.roughness_factor = 0.7f;
  if (renderer_)
    renderer_->UploadMaterial(mat);
  terrain_material_ = mat.id;

  auto assign = [&](asset::Mesh &m) {
    for (auto &lod : m.lods) {
      if (lod.submeshes.empty())
        lod.submeshes.push_back(
            asset::Submesh{0, (u32)lod.indices.size(), mat.id});
      else
        for (auto &sm : lod.submeshes)
          sm.material = mat.id;
    }
  };

  // Record id->path so .rxscene files serialize readable mesh references (the
  // procedural primitives are re-uploaded under the same ids every start).
  asset::RecordAssetPath(asset::MakeAssetId("editor/cube"), "editor/cube");
  asset::RecordAssetPath(asset::MakeAssetId("editor/sphere"), "editor/sphere");
  asset::RecordAssetPath(asset::MakeAssetId("editor/plane"), "editor/plane");

  asset::Mesh cube = asset::MakeCube(0.5f, asset::MakeAssetId("editor/cube"));
  assign(cube);
  cube_mesh_ = UploadPrimitive("cube.mesh", cube);

  asset::Mesh sphere =
      asset::MakeSphere(0.5f, 24, 32, asset::MakeAssetId("editor/sphere"));
  assign(sphere);
  sphere_mesh_ = UploadPrimitive("sphere.mesh", sphere);

  asset::Mesh plane =
      asset::MakeBox(6.0f, 0.05f, 6.0f, asset::MakeAssetId("editor/plane"));
  assign(plane);
  plane_mesh_ = UploadPrimitive("plane.mesh", plane);

  SetupDefaultTerrain();
  auto surface_y = [&](f32 x, f32 z) {
    return terrain_.SampleHeight(x, z).value_or(0.0f) + 0.5f;
  };
  ecs::Entity cube_e = SpawnMesh("cube.mesh", cube_mesh_,
                                 {-1.2f, surface_y(-1.2f, 0), 0}, "Cube");
  tints_[cube_e.index] = 0xc9a25a;
  ecs::Entity sphere_e = SpawnMesh("sphere.mesh", sphere_mesh_,
                                   {1.2f, surface_y(1.2f, 0), 0}, "Sphere");
  tints_[sphere_e.index] = 0x6a9ad6;
  ecs::Entity cube2 = SpawnMesh("cube.mesh", cube_mesh_,
                                {0, surface_y(0, -1.6f), -1.6f}, "Prop");
  tints_[cube2.index] = 0x8ac98a;

  selection_.Set(cube_e);
}

asset::AssetId Editor::UploadPrimitive(const std::string &name,
                                       const asset::Mesh &mesh) {
  asset::Mesh copy = mesh; // keep a CPU copy for picking
  if (renderer_)
    renderer_->UploadMesh(copy);
  meshes_[copy.id.hash] = MeshRecord{copy, name};
  return copy.id;
}

ecs::Entity Editor::SpawnMesh(const std::string &mesh_name, asset::AssetId mesh,
                              const Vec3 &pos, const std::string &label) {
  ecs::Entity e = world_->Create();
  world_->Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
  world_->Add(e, scene::Renderable{mesh});
  edit::EnsureGuid(*world_, e);
  SetName(e, label);
  (void)mesh_name;
  return e;
}

void Editor::SetName(ecs::Entity e, const std::string &name) {
  if (scene::Name *n = world_->Get<scene::Name>(e))
    n->value = name;
  else
    world_->Add(e, scene::Name{name});
}

std::string Editor::GetName(ecs::Entity e) const {
  if (scene::Name *n = world_->Get<scene::Name>(e))
    return n->value;
  return "";
}

void Editor::ScanAssets() {
  assets_list_.clear();
  // Built-in primitives always available.
  assets_list_.push_back({"assets://meshes/cube.mesh", "cube.mesh", "mesh"});
  assets_list_.push_back(
      {"assets://meshes/sphere.mesh", "sphere.mesh", "mesh"});
  assets_list_.push_back({"assets://meshes/plane.mesh", "plane.mesh", "mesh"});

  // Loose files under the asset root, if it exists.
  if (fs::exists(asset_root_)) {
    for (auto &p : fs::recursive_directory_iterator(asset_root_)) {
      if (!p.is_regular_file())
        continue;
      std::string ext = p.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      std::string kind;
      if (ext == ".rxscene")
        kind = "scene";
      else if (ext == ".rxterrain")
        kind = "terrain";
      else if (ext == ".gltf" || ext == ".glb")
        kind = "mesh";
      else if (asset::IsUsdPath(ext))
        kind = "stage";
      else if (ext == ".blend")
        kind = "model";
      else if (ext == ".png" || ext == ".jpg" || ext == ".dds" || ext == ".ktx")
        kind = "texture";
      else if (ext == ".mtl" || ext == ".mtlx" || ext == ".material")
        kind = "material";
      else if (ext == ".wav" || ext == ".ogg" || ext == ".xwm")
        kind = "audio";
      else
        continue;
      assets_list_.push_back(
          {p.path().string(), p.path().filename().string(), kind});
    }
  }
}

u32 Editor::ConfigureImportedBody(ImportedSkin *skin) {
  if (!skin)
    return 0;
  const asset::Skeleton &skeleton = skin->skeleton;
  u32 count = 0;
  auto available = [&](std::string_view name) {
    return skeleton.Find(name) >= 0;
  };
  auto add_chest = [&](const char *side, const char *driven,
                       const char *flatten, const char *hang) {
    if (!available(driven))
      return;
    const char *driver = available("chest.twk") ? "chest.twk" : driven;
    anim::BodyRegionConfig region = anim::MakeBodyRegionPreset(
        anim::BodyRegionKind::kChest, std::string("chest.") + side, driver,
        driven);
    // On this Genesis rig the chest bones are true deformation helpers.
    // Gravity/inertial displacement drives them, while source-authored shapes
    // preserve volume on compression and forward hang.
    region.frequency_hz = 2.45f;
    region.damping_ratio = 0.48f;
    region.max_translation = {0.04f, 0.06f, 0.075f};
    region.translation_gain = {1.2f, 1.2f, 1.2f};
    region.rotation_gain = {1.15f, 1.15f, 1.15f};
    region.deformation_axis = {0, -1, 0};
    region.morphs.push_back(
        {flatten, anim::BodyDeformationSignal::kImpact, 0.55f});
    region.morphs.push_back(
        {hang, anim::BodyDeformationSignal::kStretch, 0.35f});
    skin->dynamics.AddRegion(region);
    ++count;
  };
  add_chest("left", "pectoral.L", "pPBMlBreastsFlatten",
            "pPBMlBreastsHangForward");
  add_chest("right", "pectoral.R", "pPBMrBreastsFlatten",
            "pPBMrBreastsHangForward");

  // Morph-only abdomen and glutes regions use a real animated driver but do
  // not perturb the central skeleton. This lets authoring correctives respond
  // to landings without moving the whole torso/pelvis as a helper bone would.
  auto add_morph_mass = [&](anim::BodyRegionKind kind, const char *name,
                            const char *bone, const char *impact_morph,
                            const char *soft_morph) {
    if (!available(bone))
      return;
    anim::BodyRegionConfig region =
        anim::MakeBodyRegionPreset(kind, name, bone, bone);
    region.translation_gain = {};
    region.rotation_gain = {};
    region.frequency_hz *= 0.86f;
    region.damping_ratio = std::max(0.5f, region.damping_ratio - 0.16f);
    region.morphs.push_back(
        {impact_morph, anim::BodyDeformationSignal::kImpact, 0.38f});
    region.morphs.push_back(
        {soft_morph, anim::BodyDeformationSignal::kSpeed, 0.14f});
    skin->dynamics.AddRegion(region);
    ++count;
  };
  add_morph_mass(anim::BodyRegionKind::kAbdomen, "abdomen", "spine-1.twk",
                 "pJCMAbdomenFwd_35", "PBMStomachSoften");
  add_morph_mass(anim::BodyRegionKind::kGlutes, "glutes.left", "pelvis.twk",
                 "pJCMFlexGluteClench_L", "PBMGluteCreaseL");
  add_morph_mass(anim::BodyRegionKind::kGlutes, "glutes.right", "pelvis.twk",
                 "pJCMFlexGluteClench_R", "PBMGluteCreaseR");

  // Genesis exposes deforming tweak bones for the fleshy parts of each limb.
  // They are safe secondary-motion targets: unlike rotating the main thigh or
  // arm, moving them does not drag the entire downstream limb off its pose.
  auto add_limb = [&](anim::BodyRegionKind kind, const char *name,
                      const char *bone, const char *morph) {
    if (!available(bone))
      return;
    anim::BodyRegionConfig region =
        anim::MakeBodyRegionPreset(kind, name, bone, bone);
    // Preview tuning is intentionally readable at turntable distance while
    // remaining inside the anatomical clamps supplied by each preset.
    region.frequency_hz *= 0.82f;
    region.damping_ratio = std::max(0.52f, region.damping_ratio - 0.17f);
    region.translation_gain = {1.25f, 1.25f, 1.25f};
    region.rotation_gain = {1.2f, 1.2f, 1.2f};
    if (morph && *morph) {
      region.morphs.push_back(
          {morph, anim::BodyDeformationSignal::kImpact, 0.28f});
    }
    skin->dynamics.AddRegion(region);
    ++count;
  };
  add_limb(anim::BodyRegionKind::kThigh, "thigh.left",
           "thigh.bend.twk.L", "pJCMThighBack_35_L");
  add_limb(anim::BodyRegionKind::kThigh, "thigh.right",
           "thigh.bend.twk.R", "pJCMThighBack_35_R");
  add_limb(anim::BodyRegionKind::kCalf, "calf.left", "shin.bend.twk.L",
           "pJCMFlexCalf_L");
  add_limb(anim::BodyRegionKind::kCalf, "calf.right", "shin.bend.twk.R",
           "pJCMFlexCalf_R");
  add_limb(anim::BodyRegionKind::kUpperArm, "upper-arm.left",
           "upper_arm.bend.twk.L", "pJCMFlexBiceps_L");
  add_limb(anim::BodyRegionKind::kUpperArm, "upper-arm.right",
           "upper_arm.bend.twk.R", "pJCMFlexBiceps_R");
  return count;
}

bool Editor::LoadModelDocument(const std::string &path) {
  std::string load_path = path;
  bool reused_cache = false;
  if (fs::path(path).extension() == ".blend") {
    asset::BlendImportOptions options;
    options.converter_script = RX_BLEND_CONVERTER_SCRIPT;
    asset::BlendImportResult converted;
    std::string error;
    if (!asset::ConvertBlendScene(path, options, &converted, &error)) {
      status_message_ = "Blend import failed: " + error;
      RX_WARN("editor: {}", status_message_);
      MarkDirty();
      return false;
    }
    load_path = converted.glb_path;
    reused_cache = converted.reused_cache;
  }

  asset::ImportedScene imported_scene;
  const bool imported = asset::IsUsdPath(load_path)
                            ? asset::LoadUsdScene(load_path, &imported_scene)
                            : asset::LoadGltfScene(load_path, &imported_scene);
  if (!imported) {
    status_message_ = "Model load failed: " + path;
    MarkDirty();
    return false;
  }
  if (renderer_) {
    for (const asset::Texture &texture : imported_scene.textures)
      if (texture.id)
        renderer_->UploadTexture(texture);
    for (const asset::Material &material : imported_scene.materials)
      renderer_->UploadMaterial(material);
  }
  for (u32 i = 0; i < imported_scene.meshes.size(); ++i) {
    UploadPrimitive(fs::path(path).filename().string() + "#" +
                        std::to_string(i),
                    imported_scene.meshes[i]);
  }

  ImportedModel model;
  model.source_path = path;
  model.skins.resize(imported_scene.skeletons.size());
  u32 region_count = 0;
  for (u32 i = 0; i < imported_scene.skeletons.size(); ++i) {
    ImportedSkin &skin = model.skins[i];
    skin.skeleton = std::move(imported_scene.skeletons[i]);
    if (i < imported_scene.skin_bindings.size())
      skin.binding = std::move(imported_scene.skin_bindings[i]);
    skin.pose.ResetToBind(skin.skeleton);
    region_count += ConfigureImportedBody(&skin);
  }

  const u32 model_index = static_cast<u32>(imported_models_.size());
  for (const asset::ImportedScene::Instance &source : imported_scene.instances) {
    if (source.mesh_index >= imported_scene.meshes.size())
      continue;
    scene::Transform transform;
    transform.position[0] = source.position.x;
    transform.position[1] = source.position.y;
    transform.position[2] = source.position.z;
    std::copy(std::begin(source.rotation), std::end(source.rotation),
              transform.rotation);
    transform.scale = source.scale;
    ecs::Entity entity = world_->Create();
    world_->Add(entity, transform);
    world_->Add(entity,
                scene::Renderable{imported_scene.meshes[source.mesh_index].id});
    world_->Add(entity, scene::Transient{});
    SetName(entity, fs::path(path).stem().string() + " " +
                        std::to_string(model.instances.size() + 1));

    ImportedInstance instance;
    instance.entity = entity;
    instance.mesh = imported_scene.meshes[source.mesh_index].id.hash;
    instance.skin = source.skeleton_index;
    instance.turntable_position = source.position;
    instance.turntable_rotation = {source.rotation[0], source.rotation[1],
                                   source.rotation[2], source.rotation[3]};
    instance.rotates_with_turntable = source.skeleton_index >= 0;
    if (instance.skin >= 0 &&
        instance.skin < static_cast<i32>(model.skins.size())) {
      instance.remap =
          anim::BuildBoneRemap(model.skins[instance.skin].skeleton,
                               model.skins[instance.skin].binding);
    }
    const u32 instance_index = static_cast<u32>(model.instances.size());
    model.instances.push_back(std::move(instance));
    imported_entities_[ImportedEntityKey(entity)] = {model_index,
                                                     instance_index};
  }
  imported_models_.push_back(std::move(model));

  // Frame character imports from the skinned pieces. Authoring scenes often
  // include a huge cyclorama or ground plane; including those static props in
  // the focus bounds makes the actual character microscopic in the viewport.
  const bool has_skinned_instances = std::any_of(
      imported_scene.instances.begin(), imported_scene.instances.end(),
      [](const asset::ImportedScene::Instance &instance) {
        return instance.skeleton_index >= 0;
      });
  Vec3 center{};
  u32 centers = 0;
  for (const asset::ImportedScene::Instance &instance : imported_scene.instances) {
    if (instance.mesh_index >= imported_scene.meshes.size())
      continue;
    if (has_skinned_instances && instance.skeleton_index < 0)
      continue;
    const asset::Mesh &mesh = imported_scene.meshes[instance.mesh_index];
    const Mat4 transform = MakeTransform(
        instance.position,
        {instance.rotation[0], instance.rotation[1], instance.rotation[2],
         instance.rotation[3]},
        instance.scale);
    center += TransformPoint(
        transform,
        {mesh.bounds_center[0], mesh.bounds_center[1], mesh.bounds_center[2]});
    ++centers;
  }
  if (centers > 0)
    center = center * (1.0f / centers);
  f32 radius = 1;
  for (const asset::ImportedScene::Instance &instance : imported_scene.instances) {
    if (instance.mesh_index >= imported_scene.meshes.size())
      continue;
    if (has_skinned_instances && instance.skeleton_index < 0)
      continue;
    const asset::Mesh &mesh = imported_scene.meshes[instance.mesh_index];
    const Mat4 transform = MakeTransform(
        instance.position,
        {instance.rotation[0], instance.rotation[1], instance.rotation[2],
         instance.rotation[3]},
        instance.scale);
    const Vec3 instance_center = TransformPoint(
        transform,
        {mesh.bounds_center[0], mesh.bounds_center[1], mesh.bounds_center[2]});
    radius = std::max(radius, Length(instance_center - center) +
                                  mesh.bounds_radius * instance.scale);
  }
  ImportedModel &stored_model = imported_models_.back();

  // Give character imports a dedicated studio turntable. Its top sits at the
  // lowest skinned bound and it rotates in lockstep with every skinned piece.
  if (has_skinned_instances) {
    f32 floor_y = std::numeric_limits<f32>::max();
    for (const asset::ImportedScene::Instance &instance : imported_scene.instances) {
      if (instance.mesh_index >= imported_scene.meshes.size() ||
          instance.skeleton_index < 0)
        continue;
      const asset::Mesh &mesh = imported_scene.meshes[instance.mesh_index];
      const Mat4 transform = MakeTransform(
          instance.position,
          {instance.rotation[0], instance.rotation[1], instance.rotation[2],
           instance.rotation[3]},
          instance.scale);
      if (mesh.lods.empty())
        continue;
      for (const asset::Vertex &vertex : mesh.lods[0].vertices) {
        floor_y = std::min(
            floor_y,
            TransformPoint(transform, {vertex.position[0], vertex.position[1],
                                       vertex.position[2]})
                .y);
      }
    }
    if (!std::isfinite(floor_y))
      floor_y = center.y - radius;
    constexpr f32 kPlateHalfHeight = 0.055f;
    const f32 surface_y =
        terrain_.SampleHeight(center.x, center.z).value_or(floor_y);
    const f32 plate_top = std::max(floor_y, surface_y + 0.035f);
    const f32 character_lift = plate_top - floor_y;
    center.y += character_lift;
    for (ImportedInstance &instance : stored_model.instances) {
      if (!instance.rotates_with_turntable)
        continue;
      instance.turntable_position.y += character_lift;
      if (scene::Transform *transform =
              world_->Get<scene::Transform>(instance.entity)) {
        transform->position[1] += character_lift;
      }
    }
    const f32 plate_radius = std::max(0.9f, radius * 0.72f);
    const asset::AssetId plate_material = asset::MakeAssetId(
        path + "#editor-turntable-material-" + std::to_string(model_index));
    asset::Material material;
    material.id = plate_material;
    material.base_color_factor[0] = 0.055f;
    material.base_color_factor[1] = 0.12f;
    material.base_color_factor[2] = 0.16f;
    material.base_color_factor[3] = 1;
    material.metallic_factor = 0.72f;
    material.roughness_factor = 0.24f;
    if (renderer_)
      renderer_->UploadMaterial(material);
    const asset::AssetId plate_mesh = asset::MakeAssetId(
        path + "#editor-turntable-mesh-" + std::to_string(model_index));
    UploadPrimitive("turntable.mesh",
                    MakeTurntable(plate_radius, kPlateHalfHeight, plate_mesh,
                                  plate_material));
    stored_model.turntable_entity = world_->Create();
    scene::Transform plate_transform;
    plate_transform.position[0] = center.x;
    plate_transform.position[1] = plate_top - kPlateHalfHeight;
    plate_transform.position[2] = center.z;
    world_->Add(stored_model.turntable_entity, plate_transform);
    world_->Add(stored_model.turntable_entity,
                scene::Renderable{plate_mesh});
    world_->Add(stored_model.turntable_entity, scene::Transient{});
    SetName(stored_model.turntable_entity, "Jiggle turntable");
  }
  stored_model.turntable_center = center;
  camera_.set_position(center +
                       Vec3{radius * 1.4f, radius * 0.75f, radius * 1.8f});
  Vec3 direction = Normalize(center - camera_.position());
  camera_.set_yaw_pitch(std::atan2(direction.x, -direction.z),
                        std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
  status_message_ = "Imported " + fs::path(path).filename().string() + ": " +
                    std::to_string(imported_scene.meshes.size()) + " meshes, " +
                    std::to_string(region_count) + " jiggle regions" +
                    (reused_cache ? " (cached)" : "");
  RX_INFO("editor: {}", status_message_);
  playing_ = region_count > 0; // imported characters preview immediately
  MarkDirty();
  return true;
}

void Editor::UpdateImportedModels(f32 dt) {
  for (ImportedModel &model : imported_models_) {
    const f32 preview_dt = playing_ ? std::max(dt, 0.0f) : 0.0f;
    model.preview_time += preview_dt;

    // Auto preview holds each style for five seconds and crossfades for one,
    // making their different silhouettes easy to compare without a pose pop.
    const anim::WalkStyle hip_sway =
        anim::MakeWalkStylePreset(anim::WalkStyleKind::kHipSway);
    const anim::WalkStyle march =
        anim::MakeWalkStylePreset(anim::WalkStyleKind::kMarch);
    anim::WalkStyle walk_style;
    if (walk_preview_mode_ == WalkPreviewMode::kHipSway) {
      model.active_walk_style = anim::WalkStyleKind::kHipSway;
      walk_style = hip_sway;
    } else if (walk_preview_mode_ == WalkPreviewMode::kMarch) {
      model.active_walk_style = anim::WalkStyleKind::kMarch;
      walk_style = march;
    } else {
      const f32 cycle = std::fmod(model.preview_time, 12.0f);
      if (cycle < 5.0f) {
        model.active_walk_style = anim::WalkStyleKind::kHipSway;
        walk_style = hip_sway;
      } else if (cycle < 6.0f) {
        const f32 blend = cycle - 5.0f;
        model.active_walk_style = blend < 0.5f
                                      ? anim::WalkStyleKind::kHipSway
                                      : anim::WalkStyleKind::kMarch;
        walk_style = anim::BlendWalkStyles(hip_sway, march, blend);
      } else if (cycle < 11.0f) {
        model.active_walk_style = anim::WalkStyleKind::kMarch;
        walk_style = march;
      } else {
        const f32 blend = cycle - 11.0f;
        model.active_walk_style = blend < 0.5f
                                      ? anim::WalkStyleKind::kMarch
                                      : anim::WalkStyleKind::kHipSway;
        walk_style = anim::BlendWalkStyles(march, hip_sway, blend);
      }
    }
    model.walk_phase = anim::AdvancePhase(model.walk_phase, 1.35f, preview_dt,
                                          walk_style);

    const f32 turntable_angle = model.preview_time * 0.62f;
    const Quat turntable_rotation =
        QuatFromAxisAngle({0, 1, 0}, turntable_angle);
    for (ImportedInstance &instance : model.instances) {
      if (!world_->IsAlive(instance.entity)) {
        imported_entities_.erase(ImportedEntityKey(instance.entity));
        continue;
      }
      if (!instance.rotates_with_turntable)
        continue;
      scene::Transform *transform = world_->Get<scene::Transform>(instance.entity);
      if (!transform)
        continue;
      const Vec3 position =
          model.turntable_center +
          Rotate(turntable_rotation,
                 instance.turntable_position - model.turntable_center);
      transform->position[0] = position.x;
      transform->position[1] = position.y;
      transform->position[2] = position.z;
      const Quat rotation =
          Normalize(turntable_rotation * instance.turntable_rotation);
      transform->rotation[0] = rotation.x;
      transform->rotation[1] = rotation.y;
      transform->rotation[2] = rotation.z;
      transform->rotation[3] = rotation.w;
    }
    if (scene::Transform *plate =
            world_->Get<scene::Transform>(model.turntable_entity)) {
      plate->rotation[0] = turntable_rotation.x;
      plate->rotation[1] = turntable_rotation.y;
      plate->rotation[2] = turntable_rotation.z;
      plate->rotation[3] = turntable_rotation.w;
    }
    // A repeating force-demo rig: sustained multidirectional acceleration plus
    // a distinct shove every 0.8 seconds (left, right, then a landing). This is
    // deliberately stronger than ordinary locomotion so every configured soft
    // region is easy to inspect from the editor camera.
    const i32 event = static_cast<i32>(model.preview_time / 0.8f);
    const i32 event_kind = event % 3;
    const bool fire_event = playing_ && event != model.force_event;
    for (ImportedSkin &skin : model.skins) {
      if (playing_) {
        anim::Locomotion walk;
        walk.phase = model.walk_phase;
        walk.style = walk_style;
        walk.Apply(skin.skeleton, 1.35f, &skin.pose);
      } else {
        skin.pose.ResetToBind(skin.skeleton);
      }
      anim::BodyDynamicsFrame frame;
      if (playing_) {
        frame.linear_acceleration = {
            std::sin(model.preview_time * 2.7f) * 7.0f,
            std::sin(model.preview_time * 5.4f) * 5.5f,
            std::cos(model.preview_time * 2.1f) * 6.0f,
        };
        frame.angular_acceleration = {
            std::sin(model.preview_time * 2.3f) * 2.8f,
            std::cos(model.preview_time * 1.7f) * 3.5f,
            std::sin(model.preview_time * 3.2f) * 3.0f,
        };
        if (fire_event) {
          if (event_kind == 0)
            frame.linear_impulse = {1.15f, 0.18f, -0.35f};
          else if (event_kind == 1)
            frame.linear_impulse = {-1.15f, 0.12f, 0.45f};
          else
            frame.linear_impulse = {0.25f, -1.75f, -0.25f};
          frame.angular_impulse =
              event_kind == 2 ? Vec3{0.35f, 0, -0.28f}
                              : Vec3{0, event_kind == 0 ? 0.55f : -0.55f,
                                     event_kind == 0 ? 0.25f : -0.25f};
        }
      } else {
        frame.gravity = {};
        frame.teleport = true;
      }
      skin.dynamics.Update(skin.skeleton, frame, dt, &skin.pose, &skin.morphs);
      anim::ComputeModelMatrices(skin.skeleton, skin.pose,
                                 &skin.model_matrices);
    }
    if (fire_event)
      model.force_event = event;
  }
}

const MeshRecord *Editor::FindMesh(u64 hash) const {
  auto it = meshes_.find(hash);
  return it == meshes_.end() ? nullptr : &it->second;
}

std::string Editor::EntityLabel(ecs::Entity e) const {
  std::string n = GetName(e);
  if (!n.empty())
    return n;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "Entity %u", e.index);
  return buf;
}

// ===========================================================================
// Per-frame
// ===========================================================================
void Editor::OnUpdate(f32 dt) {
  if (!window_)
    return;
  const InputState &in = window_->input();

  f32 inst = dt > 0 ? 1.0f / dt : 0.0f;
  fps_ = fps_ <= 0 ? inst : (fps_ * 0.9f + inst * 0.1f);

  if (ui_ready_) {
    UiHotReloadCheck(dt);
    UiFeedInput(dt);
  }

  bool lmb = in.button(MouseButton::kLeft);
  bool lmb_edge = lmb && !prev_lmb_;
  bool over_vp = CursorOverViewport();

  static const bool input_log = std::getenv("RX_EDITOR_INPUT_LOG") != nullptr;
  if (input_log && lmb_edge)
    RX_INFO("editor: lmb down at {:.0f},{:.0f} over_vp={}", in.mouse_x,
            in.mouse_y, over_vp);

  UpdateModeInteraction(lmb, lmb_edge);

  // ---- gizmo / scrub / pick on the primary selection ----
  if (!headless_) {
    // Scrub start: LMB pressed over an inspector number field.
    if (editor_mode_ == EditorMode::kSelect && lmb_edge && !scrub_.active &&
        !gizmo_drag_.active) {
      TryStartScrub(
          in.mouse_x); // begins a scrub if the cursor is over a number field
    }
    if (scrub_.active) {
      if (lmb)
        UpdateScrub();
      else {
        undo_.EndGroup();
        scrub_.active = false;
      }
    }

    if (editor_mode_ == EditorMode::kSelect && over_vp && !scrub_.active)
      UpdateGizmo(in.mouse_x, in.mouse_y, lmb, lmb_edge && !gizmo_drag_.active);

    // Pick when clicking empty viewport (no gizmo handle grabbed this click).
    if (editor_mode_ == EditorMode::kSelect && lmb_edge && over_vp &&
        !gizmo_drag_.active && !scrub_.active)
      BeginScenePick(in.mouse_x, in.mouse_y);
    if (editor_mode_ == EditorMode::kSelect)
      PollScenePick();
  }

  UpdateCamera(dt);

  if (std::getenv("RX_EDITOR_AUTOPILOT"))
    RunAutopilot();

  // ---- keyboard shortcuts ----
  bool ctrl = in.key(Key::kLeftCtrl);
  auto edge = [&](Key k) {
    bool now = in.key(k);
    bool e = now && !prev_key_[(int)k];
    return e;
  };
  if (ctrl && edge(Key::kS)) {
    if (scene_path_ == "untitled.rxscene")
      OpenFileDialog();
    else
      DoSave(scene_path_);
  }
  if (ctrl && edge(Key::kZ))
    PerformUndo();
  // rx's Key enum has no Y/N/O; use available keys (R = redo, B = new scene, G
  // = open).
  if (ctrl && edge(Key::kR))
    PerformRedo();
  if (ctrl && edge(Key::kB))
    NewScene();
  if (ctrl && edge(Key::kG))
    OpenFileDialog();
  if (!ctrl && edge(Key::kEscape) && editor_mode_ != EditorMode::kSelect)
    SetEditorMode(EditorMode::kSelect);
  if (!ctrl && editor_mode_ == EditorMode::kSelect && over_vp &&
      edge(Key::kDelete)) {
    if (ecs::Entity e = selection_.primary()) {
      undo_.Push(*world_, edit::MakeDestroyEntity(*world_, e));
      selection_.Clear();
      MarkDirty();
      doc_dirty_ = true;
    }
  }
  if (!ctrl && editor_mode_ == EditorMode::kSelect && over_vp && edge(Key::kF))
    FocusSelection();

  for (int k = 0; k < (int)Key::kCount; ++k)
    prev_key_[k] = in.key((Key)k);
  prev_lmb_ = lmb;
  prev_rmb_ = in.button(MouseButton::kRight);
}

void Editor::UpdateCamera(f32 dt) {
  const InputState &in = window_->input();
  bool typing =
      false; // camera only flies while RMB is held, so text fields are safe
  bool allow_mouse = CursorOverViewport() || camera_.looking();
  bool allow_keyboard = camera_.looking() && !typing;
  camera_.Update(in, *actions_, allow_mouse, allow_keyboard, dt);
  window_->SetRelativeMouseMode(camera_.looking());
}

// The cursor is already in pixels; the panel constants are authored at 1x and
// ugui scales its own pixel sizes by the same density, so they have to be
// scaled to stay on top of what was actually drawn.
bool Editor::CursorOverViewport() const {
  if (!window_)
    return false;
  const InputState &in = window_->input();
  const f32 s = window_->pixel_density();
  f32 W = (f32)window_->width(), H = (f32)window_->height();
  return in.mouse_x >= kLeftPanel * s && in.mouse_x <= W - kRightPanel * s &&
         in.mouse_y >= kViewportTop * s && in.mouse_y <= H - kBottomPanels * s;
}

void Editor::FocusSelection() {
  ecs::Entity e = selection_.primary();
  if (!e)
    return;
  scene::Transform t = edit::WorldTransform(*world_, e);
  Vec3 center{t.position[0], t.position[1], t.position[2]};
  Vec3 eye = center - camera_.forward() * 4.0f;
  camera_.set_position(eye);
  Vec3 d = Normalize(center - eye);
  camera_.set_yaw_pitch(std::atan2(d.x, -d.z),
                        std::asin(std::clamp(d.y, -1.0f, 1.0f)));
}

// ===========================================================================
// Picking. The engine GPU path (DrawItem::pick_id + Renderer::RequestPick /
// TakePickResult) is the default; the CPU ray-vs-mesh raycast stays compiled
// as the fallback behind the flag.
// ===========================================================================
#define RX_EDITOR_HAVE_ENGINE_PICKING 1

void Editor::BeginScenePick(f32 mx, f32 my) {
#if RX_EDITOR_HAVE_ENGINE_PICKING
  if (renderer_) {
    renderer_->RequestPick((u32)mx, (u32)my);
    pick_pending_ = true;
    return;
  }
#endif
  ecs::Entity hit = PickAt(mx, my);
  if (hit)
    selection_.Set(hit);
  else
    selection_.Clear();
  MarkDirty();
}

void Editor::PollScenePick() {
  if (!pick_pending_ || !renderer_)
    return;
  if (auto result = renderer_->TakePickResult()) {
    pick_pending_ = false;
    auto it = pick_map_.find(result->pick_id);
    if (it != pick_map_.end() && world_->IsAlive(it->second)) {
      selection_.Set(it->second);
      RX_INFO("editor: pick id {} -> {}", result->pick_id,
              EntityLabel(it->second));
    } else {
      selection_.Clear(); // background
      RX_INFO("editor: pick id {} -> background", result->pick_id);
    }
    MarkDirty();
  }
}

namespace {
bool RayTriangle(const Vec3 &o, const Vec3 &d, const Vec3 &a, const Vec3 &b,
                 const Vec3 &c, f32 *t_out) {
  Vec3 e1 = b - a, e2 = c - a;
  Vec3 p = Cross(d, e2);
  f32 det = Dot(e1, p);
  if (std::fabs(det) < 1e-8f)
    return false;
  f32 inv = 1.0f / det;
  Vec3 tv = o - a;
  f32 u = Dot(tv, p) * inv;
  if (u < 0 || u > 1)
    return false;
  Vec3 q = Cross(tv, e1);
  f32 v = Dot(d, q) * inv;
  if (v < 0 || u + v > 1)
    return false;
  f32 t = Dot(e2, q) * inv;
  if (t <= 1e-4f)
    return false;
  *t_out = t;
  return true;
}
} // namespace

ecs::Entity Editor::PickAt(f32 mx, f32 my) const {
  auto [origin, dir] = ViewportCameraRay(mx, my);

  f32 best_t = 1e30f;
  ecs::Entity best = ecs::kInvalidEntity;
  world_->Each<scene::Transform, scene::Renderable>([&](ecs::Entity e,
                                                        scene::Transform &,
                                                        scene::Renderable &r) {
    if (world_->Has<scene::Hidden>(e) || IsTerrainVisual(e))
      return;
    const MeshRecord *rec = FindMesh(r.mesh.hash);
    if (!rec || rec->mesh.lods.empty())
      return;
    scene::Transform wt = edit::WorldTransform(*world_, e);
    Mat4 world = MatOf(wt);
    // Broad phase: bounding sphere.
    Vec3 c = TransformPoint(world, {rec->mesh.bounds_center[0],
                                    rec->mesh.bounds_center[1],
                                    rec->mesh.bounds_center[2]});
    f32 radius = rec->mesh.bounds_radius * wt.scale * 1.05f + 0.05f;
    Vec3 oc = origin - c;
    f32 b = Dot(oc, dir);
    f32 cc = Dot(oc, oc) - radius * radius;
    if (cc > 0 && b > 0)
      return;
    if (b * b - cc < 0)
      return;
    // Narrow phase in local space (t stays the world-space distance).
    Mat4 inv = Inverse(world);
    Vec3 lo = TransformPoint(inv, origin);
    Vec3 ld = TransformDir(inv, dir);
    const auto &lod = rec->mesh.lods[0];
    for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
      const auto &va = lod.vertices[lod.indices[i]];
      const auto &vb = lod.vertices[lod.indices[i + 1]];
      const auto &vc = lod.vertices[lod.indices[i + 2]];
      f32 t;
      if (RayTriangle(lo, ld, {va.position[0], va.position[1], va.position[2]},
                      {vb.position[0], vb.position[1], vb.position[2]},
                      {vc.position[0], vc.position[1], vc.position[2]}, &t)) {
        if (t < best_t) {
          best_t = t;
          best = e;
        }
      }
    }
  });
  return best;
}

// ===========================================================================
// Gizmo: 3D axis lines through FrameView::debug_lines_overlay + ugui handle
// dots (screen-space) for hit-testing.
// ===========================================================================
Mat4 Editor::ViewMatrix() const {
  return LookAt(camera_.position(), camera_.target(), {0, 1, 0});
}
Mat4 Editor::ProjMatrix() const {
  f32 aspect = (f32)window_->width() / (f32)window_->height();
  return PerspectiveReversedZ(1.0472f, aspect, 0.1f);
}

Vec2 Editor::ProjectToScreen(const Vec3 &world, bool *in_front) const {
  Mat4 vp = ProjMatrix() * ViewMatrix();
  f32 x = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8] * world.z + vp.m[12];
  f32 y = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9] * world.z + vp.m[13];
  f32 w = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];
  if (in_front)
    *in_front = w > 1e-4f;
  if (std::fabs(w) < 1e-6f)
    w = 1e-6f;
  f32 ndc_x = x / w, ndc_y = y / w;
  return {(ndc_x * 0.5f + 0.5f) * window_->width(),
          (ndc_y * 0.5f + 0.5f) * window_->height()};
}

void Editor::UpdateGizmo(f32 mx, f32 my, bool lmb_down, bool lmb_edge) {
  ecs::Entity e = selection_.primary();
  if (!e || gizmo_mode_ != GizmoMode::kTranslate) {
    if (gizmo_drag_.active)
      undo_.EndGroup();
    gizmo_drag_.active = false;
    return;
  }
  scene::Transform *lt = world_->Get<scene::Transform>(e);
  if (!lt)
    return;
  scene::Transform wt = edit::WorldTransform(*world_, e);
  Vec3 origin{wt.position[0], wt.position[1], wt.position[2]};

  if (gizmo_drag_.active) {
    if (!lmb_down) {
      undo_.EndGroup();
      gizmo_drag_.active = false;
      return;
    }
    f32 dx = mx - gizmo_drag_.grab_mouse_x;
    f32 dy = my - gizmo_drag_.grab_mouse_y;
    f32 along = (dx * gizmo_drag_.axis_screen_dir.x +
                 dy * gizmo_drag_.axis_screen_dir.y);
    f32 world_delta = along * gizmo_drag_.world_per_pixel;
    Vec3 axis{gizmo_drag_.axis == 0 ? 1.0f : 0.0f,
              gizmo_drag_.axis == 1 ? 1.0f : 0.0f,
              gizmo_drag_.axis == 2 ? 1.0f : 0.0f};
    Vec3 np = gizmo_drag_.base_pos + axis * world_delta;
    const edit::ComponentDesc *comp = edit::FindComponentByName("Transform");
    if (comp) {
      undo_.Push(*world_,
                 edit::MakeSetProp(*world_, e, *comp, comp->props[0],
                                   edit::PropValue::Vec3(np.x, np.y, np.z)));
      doc_dirty_ = true;
    }
    return;
  }

  // Not dragging: on click, test the three axis-handle tips.
  if (lmb_edge) {
    f32 len = std::max(0.5f, Length(origin - camera_.position()) * 0.18f);
    for (int a = 0; a < 3; ++a) {
      Vec3 axis{a == 0 ? 1.0f : 0.0f, a == 1 ? 1.0f : 0.0f,
                a == 2 ? 1.0f : 0.0f};
      bool f0, f1;
      Vec2 s0 = ProjectToScreen(origin, &f0);
      Vec2 s1 = ProjectToScreen(origin + axis * len, &f1);
      if (!f1)
        continue;
      // distance from cursor to the handle tip. The tolerance is authored at
      // 1x and both operands are pixels, so it scales with the buffer or the
      // handle gets relatively harder to grab the denser the display.
      f32 d = std::hypot(mx - s1.x, my - s1.y);
      if (d < 14.0f * window_->pixel_density()) {
        gizmo_drag_.active = true;
        gizmo_drag_.axis = a;
        gizmo_drag_.base_pos =
            Vec3{lt->position[0], lt->position[1], lt->position[2]};
        gizmo_drag_.grab_mouse_x = mx;
        gizmo_drag_.grab_mouse_y = my;
        Vec2 sd{s1.x - s0.x, s1.y - s0.y};
        f32 sl = std::max(1.0f, std::hypot(sd.x, sd.y));
        gizmo_drag_.axis_screen_dir = {sd.x / sl, sd.y / sl};
        gizmo_drag_.world_per_pixel = len / sl;
        undo_.BeginGroup("Move");
        return;
      }
    }
  }
}

// ===========================================================================
// File ops
// ===========================================================================
void Editor::NewScene() {
  FinishTerrainStroke();
  FinishPlacementDrag();
  if (scrub_.active)
    undo_.EndGroup();
  scrub_ = {};
  if (gizmo_drag_.active)
    undo_.EndGroup();
  gizmo_drag_.active = false;
  ClearTerrainVisuals();
  std::vector<ecs::Entity> all;
  world_->Each<scene::Transform>(
      [&](ecs::Entity e, scene::Transform &) { all.push_back(e); });
  for (ecs::Entity e : all)
    world_->Destroy(e);
  imported_models_.clear();
  imported_entities_.clear();
  undo_.Clear();
  selection_.Clear();
  tints_.clear();
  SetupDefaultScene();
  scene_path_ = "untitled.rxscene";
  terrain_path_ = "untitled.rxterrain";
  doc_dirty_ = false;
  terrain_dirty_ = false;
  editor_mode_ = EditorMode::kSelect;
  placement_ = {};
  status_message_ = "New scene";
  MarkDirty();
}

void Editor::OpenDocument(const std::string &path) {
  std::string extension = fs::path(path).extension().string();
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (extension == ".rxscene") {
    DoLoad(path);
  } else if (extension == ".gltf" || extension == ".glb" ||
             extension == ".blend" || asset::IsUsdPath(path)) {
    LoadModelDocument(path);
  } else {
    status_message_ = "Unsupported document: " + path;
    RX_WARN("editor: {}", status_message_);
    MarkDirty();
  }
}

void Editor::DoSave(const std::string &path) {
  FinishTerrainStroke();
  FinishPlacementDrag();
  if (scrub_.active)
    undo_.EndGroup();
  scrub_ = {};
  if (gizmo_drag_.active)
    undo_.EndGroup();
  gizmo_drag_.active = false;
  const fs::path scene_path(path);
  const fs::path terrain_path = fs::path(path).replace_extension(".rxterrain");
  const fs::path scene_stage = scene_path.string() + ".editor-stage";
  const fs::path terrain_stage = terrain_path.string() + ".editor-stage";
  auto backup_path = [](const fs::path &target) {
    fs::path candidate = target.string() + ".editor-backup";
    for (u32 suffix = 1; fs::exists(candidate); ++suffix)
      candidate = target.string() + ".editor-backup." + std::to_string(suffix);
    return candidate;
  };
  const fs::path scene_backup = backup_path(scene_path);
  const fs::path terrain_backup = backup_path(terrain_path);
  std::error_code ignored;
  for (const fs::path &temporary : {scene_stage, terrain_stage}) {
    fs::remove(temporary, ignored);
  }

  // Terrain tiles are transient visualization entities. The authored data is
  // the sidecar, so omit those entities from the generic scene serializer.
  for (auto &[key, visual] : terrain_tiles_) {
    (void)key;
    if (world_->IsAlive(visual.entity))
      world_->Destroy(visual.entity);
  }
  std::string scene_error;
  bool scene_saved =
      edit::SaveScene(*world_, scene_stage.string(), &scene_error);
  for (auto &[key, visual] : terrain_tiles_)
    visual.entity = SpawnTerrainTile(key, visual.mesh);

  const bool has_terrain = static_cast<bool>(terrain_.desc().id);
  std::string terrain_error;
  bool terrain_saved =
      !has_terrain ||
      terrain::SaveTerrain(terrain_, terrain_stage.string(), &terrain_error);

  auto move = [](const fs::path &from, const fs::path &to, std::string *error) {
    std::error_code filesystem_error;
    fs::rename(from, to, filesystem_error);
    if (!filesystem_error)
      return true;
    if (error)
      *error = filesystem_error.message();
    return false;
  };
  bool scene_backed_up = false;
  bool terrain_backed_up = false;
  bool scene_committed = false;
  if (scene_saved && terrain_saved) {
    scene_backed_up =
        fs::exists(scene_path) && move(scene_path, scene_backup, &scene_error);
    if (fs::exists(scene_path) && !scene_backed_up)
      scene_saved = false;
    // Only a document that owns terrain may touch the sidecar. Without this
    // gate a no-terrain document saved onto a stem with an existing sidecar
    // would move it to the backup, then delete the backup on success --
    // silently destroying the user's terrain.
    if (has_terrain) {
      terrain_backed_up = fs::exists(terrain_path) &&
                          move(terrain_path, terrain_backup, &terrain_error);
      if (fs::exists(terrain_path) && !terrain_backed_up)
        terrain_saved = false;
    } else if (fs::exists(terrain_path)) {
      RX_WARN("editor: leaving unrelated terrain sidecar untouched: {}",
              terrain_path.string());
    }
  }
  if (scene_saved && terrain_saved) {
    scene_committed = move(scene_stage, scene_path, &scene_error);
    scene_saved = scene_committed;
  }
  if (scene_saved && terrain_saved && has_terrain)
    terrain_saved = move(terrain_stage, terrain_path, &terrain_error);

  if (!scene_saved || !terrain_saved) {
    if (scene_committed) {
      std::error_code remove_error;
      fs::remove(scene_path, remove_error);
      if (remove_error) {
        scene_saved = false;
        scene_error =
            "rollback could not remove new scene: " + remove_error.message();
      }
    }
    if (has_terrain && fs::exists(terrain_path) && terrain_backed_up) {
      std::error_code remove_error;
      fs::remove(terrain_path, remove_error);
      if (remove_error) {
        terrain_saved = false;
        terrain_error =
            "rollback could not remove new terrain: " + remove_error.message();
      }
    }
    if (scene_backed_up && !move(scene_backup, scene_path, &scene_error)) {
      scene_saved = false;
      scene_error = "rollback failed; backup preserved at " +
                    scene_backup.string() + ": " + scene_error;
    }
    if (terrain_backed_up &&
        !move(terrain_backup, terrain_path, &terrain_error)) {
      terrain_saved = false;
      terrain_error = "rollback failed; backup preserved at " +
                      terrain_backup.string() + ": " + terrain_error;
    }
  }
  for (const fs::path &temporary : {scene_stage, terrain_stage}) {
    fs::remove(temporary, ignored);
  }
  if (scene_saved && terrain_saved) {
    fs::remove(scene_backup, ignored);
    fs::remove(terrain_backup, ignored);
  }

  if (scene_saved && terrain_saved) {
    scene_path_ = path;
    terrain_path_ = terrain_path.string();
    doc_dirty_ = false;
    terrain_dirty_ = false;
    status_message_ = has_terrain ? "Saved scene + terrain" : "Saved scene";
    RX_INFO("editor: saved {}{}", path,
            has_terrain ? " and " + terrain_path.string()
                        : " without a terrain sidecar");
    ScanAssets();
  } else {
    doc_dirty_ = true;
    terrain_dirty_ = has_terrain;
    status_message_ = "Save failed:";
    if (!scene_saved)
      status_message_ += " scene " + scene_error;
    if (!terrain_saved)
      status_message_ += " terrain " + terrain_error;
    RX_WARN("editor: {}", status_message_);
  }
  MarkDirty();
}

void Editor::DoLoad(const std::string &path) {
  FinishTerrainStroke();
  FinishPlacementDrag();
  if (gizmo_drag_.active)
    undo_.EndGroup();
  gizmo_drag_.active = false;
  if (scrub_.active)
    undo_.EndGroup();
  scrub_ = {};
  if (!assets_) {
    RX_WARN("editor: no asset database (headless vfs?); cannot load scenes");
    return;
  }
  const fs::path terrain_path = fs::path(path).replace_extension(".rxterrain");
  const bool has_terrain = fs::exists(terrain_path);
  terrain::Terrain loaded_terrain;
  std::string terrain_error;
  if (has_terrain && !terrain::LoadTerrain(terrain_path.string(),
                                           &loaded_terrain, &terrain_error)) {
    status_message_ = "Terrain load failed: " + terrain_error;
    RX_WARN("editor: {}", status_message_);
    MarkDirty();
    return;
  }

  // The engine LoadScene leaves existing entities untouched; replacing the open
  // document is editor policy, so clear the current scene entities first.
  std::vector<ecs::Entity> old;
  world_->Each<scene::Transform>(
      [&](ecs::Entity e, scene::Transform &) { old.push_back(e); });
  std::string err;
  if (edit::LoadScene(*world_, *assets_, path, &err)) {
    // Resolve meshes that are not already resident (notably glTF assets placed
    // by the surface brush) before discarding the old document's entities.
    world_->Each<scene::Renderable>([&](ecs::Entity,
                                        scene::Renderable &renderable) {
      if (FindMesh(renderable.mesh.hash))
        return;
      const auto source = asset::LookupAssetPath(renderable.mesh);
      if (!source)
        return;
      AssetEntry entry{*source, fs::path(*source).filename().string(), "mesh"};
      if (asset::AssetId resolved = ResolvePlacementMesh(entry))
        renderable.mesh = resolved;
    });
    for (ecs::Entity e : old)
      world_->Destroy(e);
    imported_models_.clear();
    imported_entities_.clear();
    ClearTerrainVisuals();
    terrain_ = has_terrain ? std::move(loaded_terrain) : terrain::Terrain{};
    terrain_brush_layer_ = 0;
    if (has_terrain)
      RebuildTerrainVisuals();
    scene_path_ = path;
    terrain_path_ = terrain_path.string();
    doc_dirty_ = false;
    terrain_dirty_ = false;
    undo_.Clear();
    selection_.Clear();
    tints_.clear();
    editor_mode_ = EditorMode::kSelect;
    placement_ = {};
    status_message_ = has_terrain ? "Loaded scene + terrain"
                                  : "Loaded scene (no terrain sidecar)";
    RX_INFO("editor: loaded {}{}", path, has_terrain ? " with terrain" : "");
  } else {
    status_message_ = "Scene load failed: " + err;
    RX_WARN("editor: {}", status_message_);
  }
  MarkDirty();
}

void Editor::PerformUndo() {
  FinishTerrainStroke();
  FinishPlacementDrag();
  if (gizmo_drag_.active)
    undo_.EndGroup();
  gizmo_drag_.active = false;
  if (scrub_.active)
    undo_.EndGroup();
  scrub_ = {};
  terrain_command_replayed_ = false;
  if (!undo_.Undo(*world_))
    return;
  if (!terrain_command_replayed_)
    doc_dirty_ = true;
  MarkDirty();
}

void Editor::PerformRedo() {
  FinishTerrainStroke();
  FinishPlacementDrag();
  if (gizmo_drag_.active)
    undo_.EndGroup();
  gizmo_drag_.active = false;
  if (scrub_.active)
    undo_.EndGroup();
  scrub_ = {};
  terrain_command_replayed_ = false;
  if (!undo_.Redo(*world_))
    return;
  if (!terrain_command_replayed_)
    doc_dirty_ = true;
  MarkDirty();
}

void Editor::OpenFileDialog() {
  dialog_files_.clear();
  auto supported = [](const fs::path &path) {
    std::string extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".rxscene" || extension == ".gltf" ||
           extension == ".glb" || extension == ".blend" ||
           asset::IsUsdPath(path.string());
  };
  if (fs::exists(asset_root_)) {
    for (auto &p : fs::recursive_directory_iterator(asset_root_))
      if (p.is_regular_file() && supported(p.path()))
        dialog_files_.push_back(p.path().string());
  }
  // Also supported documents in the working dir.
  for (auto &p : fs::directory_iterator(fs::current_path()))
    if (p.is_regular_file() && supported(p.path()))
      dialog_files_.push_back(p.path().filename().string());
  dialog_open_ = true;
  MarkDirty();
}

// ===========================================================================
// Autopilot (RX_EDITOR_AUTOPILOT=1): drives the editor's own interaction code
// paths at fixed frames -- GPU pick round-trips at projected entity pixels,
// an undo-grouped move (the gizmo-drag path), undo/redo, and a save/new/load
// round-trip -- logging pass/fail so a headless GPU run smoke-tests the whole
// engine-integration surface without OS-synthesized input.
// ===========================================================================
void Editor::RunAutopilot() {
  static int f = 0;
  static f32 terrain_before = 0;
  static f32 terrain_after = 0;
  ++f;
  auto find_named = [&](const char *n) {
    ecs::Entity found = ecs::kInvalidEntity;
    world_->Each<scene::Name>([&](ecs::Entity e, scene::Name &nm) {
      if (nm.value == n)
        found = e;
    });
    return found;
  };
  auto pick_at_entity = [&](const char *n) {
    ecs::Entity e = find_named(n);
    if (!e) {
      RX_WARN("autopilot: no entity '{}'", n);
      return;
    }
    scene::Transform wt = edit::WorldTransform(*world_, e);
    bool in_front;
    Vec2 px = ProjectToScreen({wt.position[0], wt.position[1], wt.position[2]},
                              &in_front);
    RX_INFO("autopilot: RequestPick at {:.0f},{:.0f} (center of '{}')", px.x,
            px.y, n);
    BeginScenePick(px.x, px.y);
  };
  auto check_sel = [&](const char *expect) {
    std::string got =
        selection_.primary() ? EntityLabel(selection_.primary()) : "(none)";
    RX_INFO("autopilot: selection = '{}' expected '{}' -> {}", got, expect,
            got == expect ? "PASS" : "FAIL");
  };

  const edit::ComponentDesc *xf = edit::FindComponentByName("Transform");
  switch (f) {
  case 100:
    pick_at_entity("Cube");
    break;
  case 160:
    check_sel("Cube");
    break;
  case 200:
    pick_at_entity("Sphere");
    break;
  case 260:
    check_sel("Sphere");
    break;
  case 300:
    RX_INFO("autopilot: RequestPick at 700,150 (sky)");
    BeginScenePick(700, 150);
    break;
  case 360:
    check_sel("(none)");
    break;
  case 400: { // undo-grouped move of the Cube (the gizmo-drag path)
    ecs::Entity e = find_named("Cube");
    selection_.Set(e);
    undo_.BeginGroup("Move");
    undo_.Push(*world_,
               edit::MakeSetProp(*world_, e, *xf, xf->props[0],
                                 edit::PropValue::Vec3(-1.2f, 0.55f, 1.0f)));
    undo_.Push(*world_,
               edit::MakeSetProp(*world_, e, *xf, xf->props[0],
                                 edit::PropValue::Vec3(-1.2f, 0.55f, 2.0f)));
    undo_.EndGroup();
    doc_dirty_ = true;
    MarkDirty();
    RX_INFO("autopilot: moved Cube to z=2 (grouped)");
    break;
  }
  case 430: {
    undo_.Undo(*world_);
    scene::Transform *t = world_->Get<scene::Transform>(find_named("Cube"));
    RX_INFO("autopilot: undo -> cube z={:.2f} expected 0 -> {}",
            t ? t->position[2] : -99.f,
            (t && std::fabs(t->position[2]) < 1e-3f) ? "PASS" : "FAIL");
    MarkDirty();
    break;
  }
  case 460: {
    undo_.Redo(*world_);
    scene::Transform *t = world_->Get<scene::Transform>(find_named("Cube"));
    RX_INFO("autopilot: redo -> cube z={:.2f} expected 2 -> {}",
            t ? t->position[2] : -99.f,
            (t && std::fabs(t->position[2] - 2.0f) < 1e-3f) ? "PASS" : "FAIL");
    MarkDirty();
    break;
  }
  case 470:
    SetEditorMode(EditorMode::kTerrain);
    break;
  case 480: {
    terrain_before = terrain_.SampleHeight(0, 0).value_or(-99.0f);
    terrain::TerrainBrush brush;
    brush.mode = terrain::TerrainBrushMode::kRaise;
    brush.center_x = 0;
    brush.center_z = 0;
    brush.radius = 1.25f;
    brush.strength = 0.24f;
    brush.falloff = 1.0f;
    terrain::TerrainChange change = terrain_.ApplyBrush(brush);
    RebuildTerrainTiles(
        std::span<const terrain::TerrainTileKey>(change.dirty_tiles.data(),
                                                 change.dirty_tiles.size()),
        true);
    terrain_after = terrain_.SampleHeight(0, 0).value_or(-99.0f);
    const bool pass = !change.empty() && terrain_after > terrain_before;
    RecordTerrainChange(std::move(change), "Autopilot Terrain Stroke");
    terrain_dirty_ = true;
    RX_INFO("autopilot: terrain stroke {:.3f} -> {:.3f} -> {}", terrain_before,
            terrain_after, pass ? "PASS" : "FAIL");
    break;
  }
  case 500: {
    PerformUndo();
    const f32 height = terrain_.SampleHeight(0, 0).value_or(-99.0f);
    RX_INFO("autopilot: terrain undo height={:.3f} expected {:.3f} -> {}",
            height, terrain_before,
            std::fabs(height - terrain_before) < 1e-4f ? "PASS" : "FAIL");
    break;
  }
  case 520: {
    PerformRedo();
    const f32 height = terrain_.SampleHeight(0, 0).value_or(-99.0f);
    RX_INFO("autopilot: terrain redo height={:.3f} expected {:.3f} -> {}",
            height, terrain_after,
            std::fabs(height - terrain_after) < 1e-4f ? "PASS" : "FAIL");
    break;
  }
  case 530:
    if (!assets_list_.empty())
      ArmPlacement(assets_list_[0]);
    break;
  case 540:
    SetEditorMode(EditorMode::kTerrain);
    break;
  case 550:
    DoSave("scene_saved.rxscene");
    break;
  case 590:
    NewScene();
    RX_INFO("autopilot: new scene, {} entities", world_->entity_count());
    break;
  case 630:
    DoLoad("scene_saved.rxscene");
    break;
  case 670: {
    ecs::Entity e = find_named("Cube");
    scene::Transform *t = e ? world_->Get<scene::Transform>(e) : nullptr;
    RX_INFO("autopilot: after load cube z={:.2f} expected 2 -> {}",
            t ? t->position[2] : -99.f,
            (t && std::fabs(t->position[2] - 2.0f) < 1e-3f) ? "PASS" : "FAIL");
    const f32 height = terrain_.SampleHeight(0, 0).value_or(-99.0f);
    const bool sidecar_pass = fs::exists("scene_saved.rxterrain") &&
                              std::fabs(height - terrain_after) < 2e-3f;
    RX_INFO("autopilot: terrain sidecar height={:.3f} expected {:.3f} -> {}",
            height, terrain_after, sidecar_pass ? "PASS" : "FAIL");
    break;
  }
  case 710:
    pick_at_entity("Sphere");
    break;
  case 770:
    check_sel("Sphere");
    RX_INFO("autopilot: done");
    break;
  default:
    break;
  }
}

void Editor::OnFrameEnd() {
  // Headless-driving hooks for smoke tests:
  //   RX_EDITOR_SHOT=path        capture once at frame RX_EDITOR_SHOT_FRAME
  //   (20) RX_EDITOR_SHOT_EVERY=n     capture path.<k>.png every n frames
  //   instead RX_EDITOR_SHOT_QUIT=1      quit a few frames after the single
  //   capture RX_EDITOR_QUIT_FRAME=n     quit at frame n
  static int frames = 0;
  ++frames;
  if (const char *p = std::getenv("RX_EDITOR_SHOT"); p && renderer_) {
    int every = std::getenv("RX_EDITOR_SHOT_EVERY")
                    ? std::atoi(std::getenv("RX_EDITOR_SHOT_EVERY"))
                    : 0;
    int at = std::getenv("RX_EDITOR_SHOT_FRAME")
                 ? std::atoi(std::getenv("RX_EDITOR_SHOT_FRAME"))
                 : 20;
    if (every > 0) {
      if (frames % every == 0) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s.%03d.png", p, frames / every);
        renderer_->CaptureScreenshot(path);
      }
    } else {
      if (frames == at)
        renderer_->CaptureScreenshot(p);
      if (frames == at + 4 && std::getenv("RX_EDITOR_SHOT_QUIT"))
        host_->RequestQuit();
    }
  }
  if (const char *q = std::getenv("RX_EDITOR_QUIT_FRAME")) {
    if (frames >= std::atoi(q))
      host_->RequestQuit();
  }
}

} // namespace rx::editor
