#include "viewer.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <utility>

#include <base/option.h>

#include "anim/morph.h"
#include "asset/asset_database.h"
#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "edit/scene_io.h"
#include "scene/scene_handlers.h"

// Radiance .hdr decode for imported dome environment maps.
#include <stb_image.h>
#include "scene/components.h"

#include "demo_scenes.h"
#include "scene_authoring.h"

// Viewer lifecycle and per-frame policy: the front-door content dispatch
// (glTF scene or builtin demo), the day/night sun, the debug overlay and the
// capture hooks. The camera and its scripted drivers live in the sibling
// camera_input.cc translation unit; the subsystems and the loop live in
// app::Host.
namespace rx {
namespace {
// Viewer options. Namespace scope, so they register before the host runs
// InitOptionsFromEnv(). SunDir pins a fixed sun for headless lighting/shadow
// tests (its presence disables the world clock driving the sun); the renderer
// parses the value.
base::Option<const char*> SunDir{"sun.dir", nullptr, "RX_SUN_DIR"};
// Test/CI hook: RX_UI_SHOT=<path> grabs the frame after RX_UI_SHOT_FRAMES
// (default 30) and quits. Lets a headless GPU run capture a frame without
// driving the app. Like --shot it runs the clock in lockstep (main.cc), so the
// frame it grabs is the same one on a loaded machine as on an idle one.
base::Option<const char*> UiShot{"ui.shot", nullptr, "RX_UI_SHOT"};
base::Option<int> UiShotFrames{"ui.shot.frames", 30, "RX_UI_SHOT_FRAMES"};
// RX_UI_SHOT_SEQ treats RX_UI_SHOT as a prefix and dumps every frame as
// <prefix>_NNNN.png for RX_UI_SHOT_FRAMES frames, for assembling headless
// captures into video. The cadence is even because the capture clock is
// lockstep; RX_FIXED_DT picks a different one (e.g. to match a target fps).
base::Option<bool> UiShotSeq{"ui.shot.seq", false, "RX_UI_SHOT_SEQ"};
// Capture hook: RX_MORPH_WEIGHTS="name=w,name=w" pins named morph targets to
// fixed weights on every morphed instance (unmatched names are skipped per
// mesh), instead of the imported track / scripted sweep.
base::Option<const char*> MorphWeights{"morph.weights", nullptr, "RX_MORPH_WEIGHTS"};

// UsdLux intensities are photometric and their absolute scale is a per-DCC
// convention rather than anything the spec pins down - Omniverse authors a
// daylight sun at 15000 and a practical bulb in the millions - while the engine
// works in a small linear range (a demo campfire sits at 9). These map one to
// the other. They are options because the only way to judge them is to look at
// the result, and content varies by an order of magnitude between authoring
// tools.
base::Option<float> UsdSunScale{"usd.sun.scale", 2.7e-4f, "RX_USD_SUN_SCALE"};
base::Option<float> UsdDomeScale{"usd.dome.scale", 1.0e-5f, "RX_USD_DOME_SCALE"};
base::Option<float> UsdLightScale{"usd.light.scale", 4.0e-3f, "RX_USD_LIGHT_SCALE"};
// Cap on a single imported punctual light, so one absurd authored value cannot
// blow out the whole frame.
base::Option<float> UsdLightMax{"usd.light.max", 40.0f, "RX_USD_LIGHT_MAX"};
// Imported scenes are lit by their own rig, so the sky/atmosphere is off by
// default; set 0 to keep the procedural sky (an exterior stage wants it).
base::Option<bool> UsdInterior{"usd.interior", true, "RX_USD_INTERIOR"};
// Start from the stage's authored camera when it has one.
base::Option<bool> UsdUseCamera{"usd.camera", true, "RX_USD_CAMERA"};
// Yaw of an imported dome environment map. UsdLux does not pin which way a
// latlong map faces, so matching the source can need a turn.
base::Option<float> UsdDomeRotation{"usd.dome.rotation", 0.0f, "RX_USD_DOME_ROTATION"};
// Camera/filmic effects that add light the authored rig never described. Both
// halo hard in a dark interior with bright windows: lens flare mirrors the
// windows through screen centre as cool ghosts, and bloom blows a practical
// like the star ball into a white disc that swallows its own geometry. Off by
// default when a scene brings its own lighting; set 1 to get them back.
base::Option<bool> UsdCameraFx{"usd.camerafx", false, "RX_USD_CAMERAFX"};
// Capture hook: RX_TATTOO="fx,fy,fz,size[;...]" bakes decal layers onto the
// heaviest mesh of a --gltf scene. Anchors are fractions of that mesh's local
// bounds (0.5,0.75,0.5 is chest height, centred); each tattoo aims along the
// nearest vertex's normal, and the receiver is biased onto that vertex's UDIM
// tile so a multi-tile character body maps one zone across the whole layer.
// Exists to validate the texture-space decal path against imported characters.
base::Option<const char*> Tattoo{"tattoo", nullptr, "RX_TATTOO"};
// The ink page the tattoos sample: a raw square RGBA8 blob (side derived from
// the file size), uploaded as the decal atlas. Without one the stamps paint the
// projector's own footprint in flat colour.
base::Option<const char*> TattooAtlas{"tattoo.atlas", nullptr, "RX_TATTOO_ATLAS"};
}  // namespace

Viewer::Viewer(const EngineConfig& config) : config_(config) {}

Viewer::~Viewer() {
  if (cam_record_) {
    std::fclose(cam_record_);
    cam_record_ = nullptr;
  }
}

bool Viewer::OnInitialize(app::Services& services) {
  host_ = services.host;
  window_ = services.window;
  renderer_ = services.renderer;
  world_ = services.world;
  physics_ = services.physics;
  clock_ = services.clock;
  input_map_ = services.input_map;
  actions_ = services.actions;
  physics_entities_ = services.physics_bindings;

  // The engine owns no actions; register the viewer's set (names, folds and
  // default bindings) before the host resolves input.
  RegisterViewerInput(*input_map_);

  // When SunDir is set the world clock stops driving the day/night cycle.
  drive_sun_from_clock_ = SunDir.get() == nullptr;

  // The overlay is imgui on an SDL window; an offscreen capture run has a
  // renderer but no window, so this keys off the window, not on headless.
  if (window_) {
    if (!debug_ui_.Initialize(*window_, *renderer_, services.vfs)) {
      RX_WARN("debug ui unavailable");
    }
    debug_ui_.set_clock(clock_);  // Lighting panel scrubs the day/night cycle
  }

  // Wire the shared service bundle and build the demo subsystem.
  ctx_.config = &config_;
  ctx_.world = world_;
  ctx_.scheduler = services.scheduler;
  ctx_.renderer = renderer_;
  ctx_.camera = &camera_;
  ctx_.physics = physics_;
  ctx_.vfs = services.vfs;
  ctx_.audio = services.audio;
  ctx_.debug_ui = &debug_ui_;
  ctx_.physics_entities = physics_entities_;
  ctx_.hair_bindings = services.hair_bindings;
  ctx_.actions = actions_;
  demos_ = std::make_unique<DemoScenes>(ctx_);

  if (physics_->initialized()) CreatePhysicsCubeAsset();

  if (!config_.world_path.empty()) {
    // A baked world is not a scene: nothing is loaded whole, and what exists at
    // any moment is whatever the streamer has decided to keep.
    if (!world_stream_.Init(*services.vfs, config_.headless ? nullptr : renderer_, *world_,
                            config_.headless, config_.world_path, config_.world_name)) {
      return false;
    }
    camera_.set_position({96.0f, 26.0f, 6.0f});
    camera_.set_yaw_pitch(1.35f, -0.42f);
    camera_.speed = 24.0f;
  } else if (!config_.scene_path.empty()) {
    if (!LoadSceneFile()) return false;
  } else {
    demos_->CreateDemoScene();
  }
  // After the scene, whichever kind it was, so the override beats the authored
  // viewpoint rather than racing it.
  ApplyCameraOverride();

  StartAuthoringEndpoint();
  return true;
}

// --camera-at / --camera-look / --camera-fov. Each is independent: overriding
// only the eye keeps the scene looking at what it was authored to look at,
// which is the common case when backing off to see whether a composition still
// reads. A malformed triple is refused loudly rather than silently ignored -
// the flag exists to be typed by hand, and a run that quietly used the authored
// camera would be read as "the change did nothing".
void Viewer::ApplyCameraOverride() {
  auto triple = [](const std::string& text, Vec3* out) {
    std::istringstream in(text);
    return static_cast<bool>(in >> out->x >> out->y >> out->z);
  };
  Vec3 eye = camera_.position();
  Vec3 target = camera_.target();
  bool moved = false;
  if (!config_.camera_at.empty()) {
    if (!triple(config_.camera_at, &eye)) {
      RX_WARN("--camera-at '{}' is not three numbers; keeping the scene's eye",
              config_.camera_at);
    } else {
      moved = true;
    }
  }
  if (!config_.camera_look.empty()) {
    if (!triple(config_.camera_look, &target)) {
      RX_WARN("--camera-look '{}' is not three numbers; keeping the scene's target",
              config_.camera_look);
    } else {
      moved = true;
    }
  }
  if (moved) LookCameraAt(eye, target);
  if (config_.camera_fov > 0.0f) {
    scene_camera_fov_ = config_.camera_fov * 3.14159265f / 180.0f;
  }
}

// --authoring-endpoint only. Nothing is registered and no socket exists without
// it, because these commands rewrite the live scene (authoring/command_bridge.h
// carries the threat model).
void Viewer::StartAuthoringEndpoint() {
  if (config_.authoring_socket.empty()) return;

  scene::SetupSceneCommands(commands_);
  script_ctx_.world = world_;
  script_ctx_.symbols = &symbols_;
  script_ctx_.scratch = &script_scratch_;
  script_ctx_.log_sink = [](void*, script::ScriptStringView message) {
    RX_INFO("authoring: {}", message.view());
  };
  bridge_ = std::make_unique<authoring::CommandBridge>(commands_, script_ctx_);

  std::string error;
  if (!authoring_endpoint_.Start(config_.authoring_socket, &error)) {
    RX_ERROR("authoring endpoint: {}", error);
    bridge_.reset();
    return;
  }
  RX_INFO("authoring endpoint listening on {} ({} command(s))", config_.authoring_socket,
          commands_.size());
}

void Viewer::OnSimulate(f32 frame_delta) {
  (void)frame_delta;
  if (!bridge_) return;
  authoring_endpoint_.Poll(*bridge_);
  // Every reply the poll produced already copied the strings it needed out of
  // the scratch heap, so the whole batch is reclaimed here rather than growing
  // for the run (see HandlerContext::scratch).
  script_scratch_.Reset();
}

void Viewer::CreatePhysicsCubeAsset() {
  // A small wooden cube every scene can throw around (F key).
  asset::Material wood;
  wood.id = asset::MakeAssetId("builtin/physics_cube/material");
  wood.base_color_factor[0] = 0.42f;
  wood.base_color_factor[1] = 0.26f;
  wood.base_color_factor[2] = 0.14f;
  wood.roughness_factor = 0.75f;
  asset::Mesh cube = asset::MakeCube(0.25f, asset::MakeAssetId("builtin/physics_cube"));
  for (asset::MeshLod& lod : cube.lods) {
    for (asset::Submesh& submesh : lod.submeshes) submesh.material = wood.id;
    if (lod.submeshes.empty()) {
      lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), wood.id});
    }
  }
  physics_cube_mesh_ = cube.id;
  if (!config_.headless) {
    renderer_->UploadMaterial(wood);
    renderer_->UploadMesh(cube);
  }
}

bool Viewer::LoadRxScene() {
  // The runtime's own authoring components have to be registered before the
  // loader can resolve them by name; strict mode then rejects anything else,
  // so a misspelt component is a failed load rather than a missing object.
  RegisterSceneComponents();
  // Resolves Renderable asset paths (a shape-authored scene has none) and holds
  // the textures BuildSceneShapes synthesizes for the scene's patterns.
  asset::AssetDatabase db(*ctx_.vfs);
  std::string error;
  if (!edit::LoadScene(*world_, db, config_.scene_path, &error, /*strict=*/true)) {
    RX_ERROR("rxscene: {}", error);
    return false;
  }
  // Layout before geometry, geometry before anchors. Grids only move entities
  // the file declared, so they run before prefabs add any; anchors measure
  // built meshes, so they run after everything that builds one.
  if (!BuildSceneGrids(*world_, config_.scene_path, &error)) {
    RX_ERROR("rxscene: {}", error);
    return false;
  }
  if (!BuildScenePrefabs(*world_, config_.scene_path, &error)) {
    RX_ERROR("rxscene: {}", error);
    return false;
  }
  // After the prefabs, so a rotation one carries is resolved like an authored
  // one, and before the anchors, which stand a turned object on its turned
  // footprint.
  BuildSceneRotations(*world_);
  if (!BuildSceneShapes(*world_, db, config_.headless ? nullptr : renderer_, config_.scene_path,
                        &error)) {
    RX_ERROR("rxscene: {}", error);
    return false;
  }
  if (!BuildSceneModels(*world_, db, config_.headless ? nullptr : renderer_, config_.scene_path,
                        &error)) {
    RX_ERROR("rxscene: {}", error);
    return false;
  }
  if (!BuildSceneAnchors(*world_, config_.scene_path, &error)) {
    RX_ERROR("rxscene: {}", error);
    return false;
  }

  // A scene that stages its own sun takes it over from the day/night clock for
  // good: DriveSunFromClock would otherwise walk it back to the current hour on
  // the very next frame, and a capture whose light depends on when it was taken
  // is not the reproducible one --shot promises.
  if (ApplySceneEnvironment(*world_, &renderer_->settings())) {
    ctx_.scene_owns_sun = true;
    drive_sun_from_clock_ = false;
  }

  // Authored lights are static, so they are collected once here into the same
  // list an imported glTF/USD rig fills; OnBuildView hands it to the frame.
  world_->Each<SceneLight, scene::Transform>(
      [&](ecs::Entity, SceneLight& light, scene::Transform& transform) {
        render::PointLight out;
        out.pos_radius[0] = transform.position[0];
        out.pos_radius[1] = transform.position[1];
        out.pos_radius[2] = transform.position[2];
        out.pos_radius[3] = light.radius;
        out.color_intensity[0] = light.color[0];
        out.color_intensity[1] = light.color[1];
        out.color_intensity[2] = light.color[2];
        out.color_intensity[3] = light.intensity;
        scene_lights_.push_back(out);
      });

  u32 shapes = 0;
  world_->Each<SceneShape>([&](ecs::Entity, SceneShape&) { ++shapes; });
  u32 models = 0;
  world_->Each<SceneModel>([&](ecs::Entity, SceneModel&) { ++models; });
  bool has_camera = false;
  world_->Each<SceneCamera, scene::Transform>(
      [&](ecs::Entity, SceneCamera& camera, scene::Transform& transform) {
        if (has_camera) return;  // first one wins
        has_camera = true;
        LookCameraAt({transform.position[0], transform.position[1], transform.position[2]},
                     {camera.target[0], camera.target[1], camera.target[2]});
        scene_camera_fov_ = camera.fov_degrees * 3.14159265f / 180.0f;
      });
  if (!has_camera) {
    // No authored viewpoint: back off along +Z looking at the origin, which at
    // least puts a scene built around the origin on screen.
    LookCameraAt({0.0f, 2.0f, 8.0f}, {0.0f, 1.0f, 0.0f});
  }
  camera_.speed = 4.0f;

  RX_INFO("rxscene: loaded {} ({} shape(s), {} model(s), {} light(s), camera {})",
          config_.scene_path, shapes, models, scene_lights_.size(),
          has_camera ? "authored" : "default");
  return true;
}

bool Viewer::LoadSceneFile() {
  if (config_.scene_path.ends_with(".rxscene")) return LoadRxScene();
  asset::ImportedScene scene;
  const bool loaded = asset::IsUsdPath(config_.scene_path)
                          ? asset::LoadUsdScene(config_.scene_path, &scene,
                                                config_.usd_visibility)
                          : asset::LoadGltfScene(config_.scene_path, &scene);
  if (!loaded) return false;

  if (!config_.headless) {
    for (const asset::Texture& texture : scene.textures) {
      if (texture.id) renderer_->UploadTexture(texture);
    }
    for (const asset::Material& material : scene.materials) renderer_->UploadMaterial(material);
    for (const asset::Mesh& mesh : scene.meshes) renderer_->UploadMesh(mesh);
  }

  base::Vector<std::pair<u32, ecs::Entity>> instance_entities;
  for (const asset::ImportedScene::Instance& instance : scene.instances) {
    const asset::Mesh& mesh = scene.meshes[instance.mesh_index];
    // Morphed instances stay out of the ECS gather; EmitMorphedInstances
    // draws them with live weights (imported track or scripted sweep).
    if (!mesh.morph_targets.empty()) {
      MorphedInstance morphed;
      morphed.mesh = mesh.id.hash;
      morphed.transform =
          MakeTranslation(instance.position) *
          MakeFromQuat(instance.rotation[0], instance.rotation[1], instance.rotation[2],
                       instance.rotation[3]) *
          MakeScale(instance.scale);
      if (!mesh.morph_animations.empty()) morphed.animation = mesh.morph_animations[0];
      morphed.weights.resize(mesh.morph_targets.size());
      if (const char* spec = MorphWeights.get()) {
        // "name=w,name=w": resolve each name against this mesh's targets.
        morphed.pinned = true;
        std::string s(spec);
        size_t pos = 0;
        while (pos < s.size()) {
          size_t comma = s.find(',', pos);
          if (comma == std::string::npos) comma = s.size();
          std::string entry = s.substr(pos, comma - pos);
          pos = comma + 1;
          size_t eq = entry.find('=');
          if (eq == std::string::npos) continue;
          std::string name = entry.substr(0, eq);
          f32 weight = std::strtof(entry.c_str() + eq + 1, nullptr);
          i32 index = mesh.FindMorphTarget(asset::MakeAssetId(name).hash);
          if (index >= 0) morphed.weights[static_cast<u32>(index)] = weight;
        }
      } else if (mesh.morph_animations.empty()) {
        // No imported track: hand the instance to the expression controller
        // when its targets carry names the stock poses know (an ARKit-style
        // face; the eyelash/brow meshes share those names and follow). An
        // imported track always wins over the controller.
        if (!expression_demo_) {
          expression_.AddDefaultPoses();
          expression_.SetExpression("neutral");
        }
        u32 matched = 0;
        morphed.expression_map.resize(expression_.channel_count());
        for (u32 c = 0; c < expression_.channel_count(); ++c) {
          i32 index = mesh.FindMorphTarget(expression_.channel_target(c));
          morphed.expression_map[c] = index;
          if (index >= 0) ++matched;
        }
        if (matched == 0) {
          morphed.expression_map.clear();  // unnamed targets: keep the sweep
        } else {
          expression_demo_ = true;
        }
      }
      morphed_.push_back(std::move(morphed));
      continue;
    }
    ecs::Entity entity = world_->Create();
    scene::Transform transform;
    transform.position[0] = instance.position.x;
    transform.position[1] = instance.position.y;
    transform.position[2] = instance.position.z;
    std::memcpy(transform.rotation, instance.rotation, sizeof(transform.rotation));
    transform.scale = instance.scale;
    world_->Add(entity, transform);
    world_->Add(entity, scene::Renderable{scene.meshes[instance.mesh_index].id});
    instance_entities.push_back({instance.mesh_index, entity});
  }
  StampTattoos(scene, instance_entities);
  if (!morphed_.empty()) {
    RX_INFO("gltf: {} morphed instance(s) animated by the viewer", morphed_.size());
  }

  // Sponza-friendly start: inside the atrium looking down the long axis.
  camera_.set_position({-7.0f, 1.7f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, 0.0f);
  camera_.speed = 4.0f;

  if (!config_.headless) {
    ApplySceneLighting(scene);
    ApplySceneCamera(scene);  // after the default, so an authored one wins
  }
  return true;
}

// RX_TATTOO: bake decals onto the heaviest mesh in a --gltf scene. Anchors are
// fractions of that mesh's local bounds; each one snaps to the nearest vertex,
// so the projector sits on the surface and points along its normal without the
// caller knowing anything about the model. That vertex's uv also picks the UDIM
// tile the receiver is biased onto - a Genesis-style body lays its zones out
// across u in [0,7), and only the anchored zone can take the decal.
void Viewer::StampTattoos(const asset::ImportedScene& scene,
                          std::span<const std::pair<u32, ecs::Entity>> instances) {
  const char* spec = Tattoo.get();
  if (!spec || config_.headless || instances.empty()) return;

  // Heaviest mesh: on a character import that is the body, which is what a
  // tattoo wants and what carries the interesting uv layout.
  size_t best = 0;
  size_t best_vertices = 0;
  for (size_t i = 0; i < instances.size(); ++i) {
    const asset::Mesh& mesh = scene.meshes[instances[i].first];
    if (mesh.lods.empty()) continue;
    if (mesh.lods[0].vertices.size() > best_vertices) {
      best_vertices = mesh.lods[0].vertices.size();
      best = i;
    }
  }
  if (best_vertices == 0) return;
  const asset::Mesh& mesh = scene.meshes[instances[best].first];
  const auto& vertices = mesh.lods[0].vertices;
  // Anchors and vertices are mesh-local, but the bake rasterizes world space
  // (push.model * local). Everything below is therefore lifted through the
  // instance's world matrix; skipping that silently misses on any import whose
  // node is not identity, which is every Y-up / cm-scaled character.
  const scene::Transform* placement = world_->Get<scene::Transform>(instances[best].second);
  const Mat4 to_world = placement ? MakeTranslation({placement->position[0], placement->position[1],
                                                     placement->position[2]}) *
                                        MakeFromQuat(placement->rotation[0], placement->rotation[1],
                                                     placement->rotation[2], placement->rotation[3]) *
                                        MakeScale(placement->scale)
                                  : Mat4::Identity();

  Vec3 lo{vertices[0].position[0], vertices[0].position[1], vertices[0].position[2]};
  Vec3 hi = lo;
  for (const asset::Vertex& v : vertices) {
    lo = {std::min(lo.x, v.position[0]), std::min(lo.y, v.position[1]),
          std::min(lo.z, v.position[2])};
    hi = {std::max(hi.x, v.position[0]), std::max(hi.y, v.position[1]),
          std::max(hi.z, v.position[2])};
  }

  if (const char* atlas_path = TattooAtlas.get()) {
    std::FILE* file = std::fopen(atlas_path, "rb");
    if (!file) {
      RX_WARN("RX_TATTOO_ATLAS: cannot open {}", atlas_path);
    } else {
      std::fseek(file, 0, SEEK_END);
      const long bytes = std::ftell(file);
      std::fseek(file, 0, SEEK_SET);
      const u64 texels = bytes > 0 ? static_cast<u64>(bytes) / 4 : 0;
      const u64 side = static_cast<u64>(std::lround(std::sqrt(static_cast<f64>(texels))));
      if (bytes <= 0 || side * side * 4 != static_cast<u64>(bytes)) {
        RX_WARN("RX_TATTOO_ATLAS: {} is {} bytes, not a square rgba8 image", atlas_path, bytes);
      } else {
        asset::Texture ink;
        ink.id = asset::MakeAssetId("viewer/tattoo/ink");
        ink.format = asset::TextureFormat::kRgba8;
        ink.width = static_cast<u32>(side);
        ink.height = static_cast<u32>(side);
        ink.is_srgb = true;
        ink.data.resize(static_cast<size_t>(bytes));
        if (std::fread(ink.data.data(), 1, static_cast<size_t>(bytes), file) ==
            static_cast<size_t>(bytes)) {
          renderer_->UploadTexture(ink);
          renderer_->SetDecalAtlas(ink.id);
        } else {
          RX_WARN("RX_TATTOO_ATLAS: short read on {}; stamps fall back to the flat page",
                  atlas_path);
        }
      }
      std::fclose(file);
    }
  }

  const f32 placement_scale = placement ? placement->scale : 1.0f;

  const u32 receiver = renderer_->AcquireDecalReceiver();
  if (receiver == 0) {
    RX_WARN("RX_TATTOO: the decal baker is unavailable");
    return;
  }

  bool tile_chosen = false;
  i32 tile_u = 0, tile_v = 0;
  u32 stamped = 0;
  std::string s(spec);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t end = s.find(';', pos);
    if (end == std::string::npos) end = s.size();
    const std::string entry = s.substr(pos, end - pos);
    pos = end + 1;
    f32 f[4] = {0.5f, 0.5f, 0.5f, 0.08f};
    u32 parsed = 0;
    const char* cursor = entry.c_str();
    while (parsed < 4 && *cursor) {
      char* next = nullptr;
      // strtof returns 0 on failure, so only commit the value once the parse is
      // known good: a trailing space would otherwise zero the size and build a
      // degenerate projector (all-zero rows, NaN facing test).
      const f32 value = std::strtof(cursor, &next);
      if (next == cursor) break;
      f[parsed] = value;
      ++parsed;
      cursor = *next == ',' ? next + 1 : next;
    }
    if (parsed < 3) continue;

    const Vec3 anchor{lo.x + (hi.x - lo.x) * f[0], lo.y + (hi.y - lo.y) * f[1],
                      lo.z + (hi.z - lo.z) * f[2]};
    const asset::Vertex* nearest = nullptr;
    f32 nearest_distance = 0;
    for (const asset::Vertex& v : vertices) {
      const Vec3 delta{v.position[0] - anchor.x, v.position[1] - anchor.y,
                       v.position[2] - anchor.z};
      const f32 distance = Dot(delta, delta);
      if (!nearest || distance < nearest_distance) {
        nearest = &v;
        nearest_distance = distance;
      }
    }
    if (!nearest) continue;

    // One draw carries one layer tile, so every tattoo has to live in the zone
    // the first anchor picked; a later anchor on another zone would bake
    // outside the tile and shade nothing.
    const i32 anchor_tile_u = static_cast<i32>(std::floor(nearest->uv[0]));
    const i32 anchor_tile_v = static_cast<i32>(std::floor(nearest->uv[1]));
    if (!tile_chosen) {
      tile_u = anchor_tile_u;
      tile_v = anchor_tile_v;
      tile_chosen = true;
      renderer_->SetDecalReceiverUv(receiver, 1.0f, 1.0f, -static_cast<f32>(tile_u),
                                    -static_cast<f32>(tile_v));
    } else if (anchor_tile_u != tile_u || anchor_tile_v != tile_v) {
      RX_WARN("RX_TATTOO: anchor {},{},{} is on uv tile {},{}, not the receiver's {},{}; skipped",
              f[0], f[1], f[2], anchor_tile_u, anchor_tile_v, tile_u, tile_v);
      continue;
    }

    const Vec3 position = TransformPoint(
        to_world, {nearest->position[0], nearest->position[1], nearest->position[2]});
    const Vec3 normal = Normalize(TransformDir(
        to_world, {nearest->normal[0], nearest->normal[1], nearest->normal[2]}));
    Vec3 up{0, 1, 0};
    if (std::abs(Dot(up, normal)) > 0.9f) up = {0, 0, 1};
    render::DecalStamp tattoo;
    tattoo.receiver = receiver;
    const f32 world_size = f[3] * placement_scale;
    tattoo.projector = render::MakeDecalProjector(position, normal, up, world_size, world_size,
                                                  world_size * 2.0f);
    // Flat 2d ink: albedo only, no normal or roughness change.
    tattoo.projector.tint_blend[0] = 1.0f;
    tattoo.projector.tint_blend[1] = 1.0f;
    tattoo.projector.tint_blend[2] = 1.0f;
    tattoo.projector.tint_blend[3] = 0.95f;
    tattoo.projector.params2[0] = 0.0f;
    tattoo.projector.params2[1] = 1.0f;
    renderer_->StampDecal(tattoo);
    ++stamped;
  }

  if (stamped == 0) {
    renderer_->ReleaseDecalReceiver(receiver);
    return;
  }
  world_->Add(instances[best].second, scene::DecalReceiver{receiver});
  tattoo_receiver_ = receiver;
  RX_INFO("RX_TATTOO: {} tattoo(s) on '{}' ({} verts), uv tile {},{}", stamped,
          mesh.id.hash, best_vertices, tile_u, tile_v);
}

// World-space area of an emitter, used only to undo UsdLux `normalize` (which
// divides emitted power by area). The engine's area lights take a radiance and
// integrate the emitter's solid angle in the shader, so area is otherwise not
// part of the conversion.
static f32 EmitterArea(const asset::ImportedScene::Light& light) {
  using Kind = asset::ImportedScene::Light::Kind;
  constexpr f32 kPi = 3.14159265358979f;
  switch (light.kind) {
    case Kind::kSphere: return 4.0f * kPi * light.radius * light.radius;
    case Kind::kDisk: return kPi * light.radius * light.radius;
    case Kind::kRect: return light.width * light.height;
    case Kind::kCylinder: return 2.0f * kPi * light.radius * light.length;
    default: return 1.0f;
  }
}

void Viewer::ApplySceneLighting(const asset::ImportedScene& scene) {
  if (scene.lights.empty()) return;
  using Kind = asset::ImportedScene::Light::Kind;
  auto& s = renderer_->settings();

  const asset::ImportedScene::Light* sun = nullptr;
  const asset::ImportedScene::Light* dome = nullptr;
  u32 punctual = 0, dropped = 0, distant_count = 0, dome_count = 0;

  for (const asset::ImportedScene::Light& light : scene.lights) {
    const f32 gain = light.intensity * std::exp2(light.exposure);
    if (light.kind == Kind::kDistant) {
      // Brightest distant light wins: a rig may carry a fill as well as a key.
      ++distant_count;
      if (!sun || gain > sun->intensity * std::exp2(sun->exposure)) sun = &light;
      continue;
    }
    if (light.kind == Kind::kDome) {
      ++dome_count;
      if (!dome || gain > dome->intensity * std::exp2(dome->exposure)) dome = &light;
      continue;
    }
    render::PointLight pl;
    pl.pos_radius[0] = light.position.x;
    pl.pos_radius[1] = light.position.y;
    pl.pos_radius[2] = light.position.z;
    // color_intensity.w is a radiance: the LTC area-light path in mesh.ps
    // integrates the emitter's solid angle itself, so multiplying by area here
    // would count it twice. UsdLux `normalize` divides emitted power by area,
    // which in radiance terms is the only case that scales by 1/area.
    const f32 area = EmitterArea(light);
    const f32 radiance =
        (light.normalize && area > 1e-6f) ? gain / area : gain;
    f32 intensity = radiance * UsdLightScale.get();
    if (intensity > UsdLightMax.get()) intensity = UsdLightMax.get();
    if (intensity <= 1e-4f) {
      ++dropped;
      continue;
    }
    // Influence radius from an inverse-square falloff down to a ~1/255 cutoff,
    // clamped so a bright practical does not light the entire stage.
    pl.pos_radius[3] = std::min(30.0f, std::max(1.0f, std::sqrt(intensity * 255.0f)));
    pl.color_intensity[0] = light.color[0];
    pl.color_intensity[1] = light.color[1];
    pl.color_intensity[2] = light.color[2];
    pl.color_intensity[3] = intensity;
    pl.direction_type[0] = light.direction.x;
    pl.direction_type[1] = light.direction.y;
    pl.direction_type[2] = light.direction.z;
    switch (light.kind) {
      case Kind::kRect:
        pl.direction_type[3] = 3.0f;
        pl.params[0] = 0.5f * light.width;
        pl.params[1] = 0.5f * light.height;
        break;
      case Kind::kSphere:
      case Kind::kDisk:
      case Kind::kCylinder:
        pl.direction_type[3] = 2.0f;  // sphere area light
        pl.params[0] = std::max(0.01f, light.radius);
        break;
      default:
        pl.direction_type[3] = 0.0f;
        break;
    }
    // A ShapingAPI cone narrower than the hemisphere is a spot.
    if (light.cone_angle < 89.9f && light.kind != Kind::kRect) {
      pl.direction_type[3] = 1.0f;
      const f32 outer = light.cone_angle * 3.14159265358979f / 180.0f;
      const f32 inner = outer * (1.0f - std::clamp(light.cone_softness, 0.0f, 1.0f));
      pl.params[0] = std::cos(inner);
      pl.params[1] = std::cos(outer);
    }
    scene_lights_.push_back(pl);
    ++punctual;
  }

  // The renderer uploads only the first kMaxFrameLights and clusters only the
  // first few per cluster, both silently. Ordering brightest-first means what
  // survives a truncation is what matters most, and the count is reported.
  std::sort(scene_lights_.begin(), scene_lights_.end(),
            [](const render::PointLight &a, const render::PointLight &b) {
              return a.color_intensity[3] > b.color_intensity[3];
            });
  if (scene_lights_.size() > 256) {
    RX_WARN("usd lighting: {} punctual lights imported but the renderer uploads "
            "256 per frame; the dimmest {} will not light the scene",
            scene_lights_.size(), scene_lights_.size() - 256);
  }

  if (sun) {
    s.sun_direction = sun->direction;
    s.sun_color = {sun->color[0], sun->color[1], sun->color[2]};
    s.sun_intensity =
        sun->intensity * std::exp2(sun->exposure) * UsdSunScale.get();
    ctx_.scene_owns_sun = true;
    drive_sun_from_clock_ = false;
  }

  // A dome with an environment map becomes the actual sky: the convolutions
  // then light the scene from every direction with the map's own colour, which
  // is what balances a warm key against a cool fill. A flat ambient cannot -
  // raise it enough to lift the shadows and it flattens the whole image.
  bool dome_ibl = false;
  if (dome && !dome->texture.empty()) {
    int w = 0, h = 0, channels = 0;
    if (f32* pixels = stbi_loadf(dome->texture.c_str(), &w, &h, &channels, 4)) {
      const Vec3 tint{dome->color[0], dome->color[1], dome->color[2]};
      const f32 gain =
          dome->intensity * std::exp2(dome->exposure) * UsdDomeScale.get();
      dome_ibl = renderer_->SetEnvironmentMap(pixels, static_cast<u32>(w),
                                              static_cast<u32>(h), tint, gain,
                                              UsdDomeRotation.get());
      stbi_image_free(pixels);
      if (dome_ibl) {
        RX_INFO("usd lighting: dome envmap '{}' ({}x{}) drives ibl",
                dome->texture, w, h);
      }
    } else {
      RX_WARN("usd lighting: could not decode dome envmap '{}'", dome->texture);
    }
  }

  ApplySceneRenderSettings(scene, dome_ibl);

  // A stage viewed against its authored look wants the light its rig describes
  // and nothing painted on top. RX_LENS_FLARE / the bloom settings still win if
  // set explicitly (the renderer applies those options only when overridden).
  if (!UsdCameraFx.get()) {
    s.lens_flare = 0.0f;
    s.bloom = false;
  } else if (scene.render_settings.has_lens_flare) {
    // The source's own view on the effect, which is far subtler than the
    // engine default (0.1 against 0.06 on a very different scale).
    s.lens_flare =
        scene.render_settings.lens_flare_enabled
            ? scene.render_settings.lens_flare_scale
            : 0.0f;
  }

  if (dome_ibl) {
    // Keep the sky/IBL path, but the atmosphere model no longer applies: the
    // background is the authored map and aerial perspective would haze an
    // interior that has no atmosphere between camera and wall.
    s.interior = false;
    s.sky = true;
    s.ibl = true;
    s.clouds = false;
    s.aerial_perspective = 0.0f;
  } else if (UsdInterior.get()) {
    // The rig is the whole lighting environment: a procedural sky behind it
    // double-lights the stage and hazes interiors with aerial perspective.
    s.interior = true;
    s.sky = false;
    s.clouds = false;
    s.aerial_perspective = 0.0f;
    const f32 dome_gain =
        dome ? dome->intensity * std::exp2(dome->exposure) * UsdDomeScale.get()
             : 0.05f;
    // The dome's tint multiplies its environment map, so the fill colour is
    // both together - the tint alone is not the colour of the sky.
    const Vec3 dome_color =
        dome ? Vec3{dome->color[0] * dome->texture_average[0],
                    dome->color[1] * dome->texture_average[1],
                    dome->color[2] * dome->texture_average[2]}
             : Vec3{1.0f, 1.0f, 1.0f};
    s.interior_ambient = {dome_color.x * dome_gain, dome_color.y * dome_gain,
                          dome_color.z * dome_gain};
    s.ambient = dome_gain;
    if (sun) {
      s.interior_directional_dir = sun->direction;
      s.interior_directional_color = s.sun_color;
      s.interior_directional_intensity = s.sun_intensity;
    }
  }

  if (distant_count > 1 || dome_count > 1) {
    RX_WARN("usd lighting: {} distant and {} dome light(s) authored; USD adds "
            "them, this takes only the brightest of each",
            distant_count, dome_count);
  }
  RX_INFO(
      "usd lighting: sun {} (intensity {:.2f}), dome {} (ambient {:.3f}), "
      "{} punctual light(s){}",
      sun ? "authored" : "none", sun ? s.sun_intensity : 0.0f,
      dome ? "authored" : "none", s.ambient, punctual,
      dropped ? ", some below the visibility floor" : "");
  if (dome && !dome->texture.empty() && !dome_ibl) {
    RX_WARN("usd lighting: dome envmap '{}' could not be installed; the flat "
            "ambient above stands in for it",
            dome->texture);
  }
}

void Viewer::ApplySceneRenderSettings(const asset::ImportedScene& scene,
                                      bool dome_ibl) {
  const asset::ImportedScene::RenderSettings& rs = scene.render_settings;
  auto& s = renderer_->settings();

  // Indirect diffuse is the one that changes the picture most. A stage authored
  // around a heavily boosted bounce - NVIDIA's Attic asks for 7x - reads as a
  // flat, unlit box at 1x, because in a path-traced interior most of what
  // reaches the room is bounce rather than direct light.
  if (rs.has_indirect_scale) {
    const f32 scale = rs.indirect_enabled ? rs.indirect_scale : 0.0f;
    s.rcgi_intensity = scale;
    s.ddgi_intensity = scale;
  }

  // The flat ambient is an additive term in the source renderer. With an
  // authored dome driving real IBL the sky fill is already accounted for, so
  // adding it again would double the indirect; it stands in only when there is
  // no dome to convolve.
  if (rs.has_ambient && !dome_ibl) {
    s.ambient = rs.ambient_intensity;
    s.interior_ambient = {rs.ambient_color[0] * rs.ambient_intensity,
                          rs.ambient_color[1] * rs.ambient_intensity,
                          rs.ambient_color[2] * rs.ambient_intensity};
  }

  if (rs.has_indirect_scale || rs.has_ambient) {
    RX_INFO("usd render settings: indirect x{:.2f}{}, ambient {}",
            rs.has_indirect_scale ? s.rcgi_intensity : 1.0f,
            rs.indirect_enabled ? "" : " (disabled)",
            (rs.has_ambient && !dome_ibl) ? "applied" : "from the dome");
  }

  // Authored fog is distance-based and starts well away from the camera (the
  // Attic's begins at 10 m, about the width of the room). Both halves map onto
  // the froxel fog: the authored value is an extinction per metre like the
  // engine's, and the start distance is what keeps the haze off the near field
  // instead of veiling the whole room.
  if (rs.has_fog && rs.fog_enabled && rs.fog_density > 0.0f) {
    if (!renderer_->froxel_density_overridden()) s.froxel_density = rs.fog_density;
    if (!renderer_->froxel_start_overridden()) s.froxel_start_distance = rs.fog_start;
    RX_INFO("usd render settings: distance fog density {:.3f} from {:.1f} m",
            rs.fog_density, rs.fog_start);
  }
}

void Viewer::ApplySceneCamera(const asset::ImportedScene& scene) {
  if (!UsdUseCamera.get() || scene.cameras.empty()) return;
  const asset::ImportedScene::Camera& camera = scene.cameras[0];
  // The stored rotation is the camera's world basis; USD looks down -Z.
  const Mat4 basis = MakeFromQuat(camera.rotation[0], camera.rotation[1],
                                  camera.rotation[2], camera.rotation[3]);
  const Vec3 forward = TransformDir(basis, {0.0f, 0.0f, -1.0f});
  camera_.set_position(camera.position);
  camera_.set_yaw_pitch(std::atan2(forward.x, -forward.z),
                        std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
  camera_.speed = 4.0f;
  // Framing is the lens as much as the pose: a 18mm wide-angle stage camera
  // shows a different scene through the engine's default 60 degrees.
  scene_camera_fov_ = camera.yfov;
  RX_INFO("usd camera: eye ({:.2f} {:.2f} {:.2f}), {:.0f} deg vfov",
          camera.position.x, camera.position.y, camera.position.z,
          camera.yfov * 57.2957795f);
}

void Viewer::DriveSunFromClock() {
  // Throttled to ~0.02-hour steps so the IBL environment is not rebuilt every
  // frame for sub-degree motion.
  if (!drive_sun_from_clock_ || ctx_.scene_owns_sun) return;
  const f32 hour = clock_->hour();
  if (last_sky_hour_ >= -100.0f && std::abs(hour - last_sky_hour_) < 0.02f) return;
  last_sky_hour_ = hour;
  const SkyLighting sky = ComputeSkyLighting(hour);
  auto& s = renderer_->settings();
  s.sun_direction = sky.sun_direction;
  s.sun_intensity = sky.sun_intensity;
  s.sun_color = sky.sun_color;
  s.ambient = sky.ambient;
  s.night = sky.night;  // the moon-lit night would otherwise read as day
}

void Viewer::OnUpdate(f32 frame_delta) {
  DriveSunFromClock();
  debug_ui_.BeginFrame();
  // Before the camera moves this frame, so the streamer sees the position the
  // last frame was drawn from and its own prediction is not a frame stale.
  world_stream_.Update(frame_delta, camera_.position());
  if (world_stream_.active()) debug_ui_.set_world_status(world_stream_.StatusLine());
  demos_->Update(frame_delta);
  // The gym owns its camera + input: route input to the character controller
  // instead of the free-fly camera and let it capture the cursor for mouse look.
  if (GymDemo* gym = demos_->gym(); gym && window_) {
    const bool allow_keyboard = !debug_ui_.wants_keyboard();
    const bool allow_mouse = !debug_ui_.wants_mouse();
    gym->Update(frame_delta, window_->input(), *actions_, allow_keyboard, allow_mouse);
    window_->SetRelativeMouseMode(gym->wants_mouse_capture());
    if (actions_->pressed(Action::kToggleDebug) && allow_keyboard) debug_ui_.ToggleVisible();
    return;
  }
  // The FPS range owns its camera + input the same way the gym does: mouse look
  // through the character, mouse buttons on the trigger.
  if (ShooterDemo* shooter = demos_->shooter(); shooter && window_) {
    const bool allow_keyboard = !debug_ui_.wants_keyboard();
    const bool allow_mouse = !debug_ui_.wants_mouse();
    shooter->Update(frame_delta, window_->input(), *actions_, allow_keyboard, allow_mouse);
    window_->SetRelativeMouseMode(shooter->wants_mouse_capture());
    if (actions_->pressed(Action::kToggleDebug) && allow_keyboard) debug_ui_.ToggleVisible();
    return;
  }
  // The driving gym likewise owns its camera + input (Tab-cycled vehicles, chase
  // / free-fly camera); route input here and skip the free-fly camera.
  if (DriveDemo* drive = demos_->drive(); drive && window_) {
    const bool allow_keyboard = !debug_ui_.wants_keyboard();
    const bool allow_mouse = !debug_ui_.wants_mouse();
    drive->Update(frame_delta, window_->input(), *actions_, allow_keyboard, allow_mouse);
    window_->SetRelativeMouseMode(drive->wants_mouse_capture());
    if (actions_->pressed(Action::kToggleDebug) && allow_keyboard) debug_ui_.ToggleVisible();
    return;
  }
  UpdateCamera(frame_delta);
  // The puppet keeps the free-fly camera; it only wants raw number keys
  // (1/2/3 -> push / big-push / reset), forwarded without an early return.
  if (PuppetDemo* puppet = demos_->puppet(); puppet && window_) {
    puppet->OnInput(window_->input(), !debug_ui_.wants_keyboard());
  }
}

void Viewer::OnBuildView(f32 frame_delta, render::FrameView& view) {
  view.camera.eye = camera_.position();
  view.camera.target = camera_.target();
  // The imported rig is static, so it is rebuilt into the frame view rather
  // than re-derived; the demo scenes assign view.lights outright, so this goes
  // in first and they win on a demo (which never has an imported rig anyway).
  if (!scene_lights_.empty()) view.lights = scene_lights_;
  if (scene_camera_fov_ > 0.0f) view.camera.fov_y = scene_camera_fov_;
  demos_->EmitToView(frame_delta, view);
  world_stream_.EmitToView(view);
  EmitMorphedInstances(frame_delta, view);
  debug_ui_.Build(*renderer_, camera_, *world_, frame_delta, &view);
  demos_->ApplyRenderPolicy();
}

void Viewer::EmitMorphedInstances(f32 frame_delta, render::FrameView& view) {
  if (morphed_.empty()) return;
  morph_time_ += frame_delta;
  if (expression_demo_) {
    // Cycle the stock poses; the life layer keeps blinking through the holds.
    expression_hold_ -= frame_delta;
    if (expression_hold_ <= 0) {
      static constexpr std::string_view kCycle[] = {"neutral", "smile",  "angry",      "surprised",
                                                    "smirk",   "pucker", "eyes_closed"};
      expression_.SetExpression(kCycle[expression_pose_ % std::size(kCycle)]);
      ++expression_pose_;
      expression_hold_ = 3.0f;
    }
    expression_.Update(frame_delta);
  }
  for (MorphedInstance& instance : morphed_) {
    if (instance.pinned) {
      // RX_MORPH_WEIGHTS: weights were fixed at load; skip track/sweep.
    } else if (!instance.animation.times.empty()) {
      f32 time = instance.animation.duration > 0
                     ? std::fmod(morph_time_, instance.animation.duration)
                     : 0.0f;
      anim::SampleMorphWeights(instance.animation, time, &instance.weights);
    } else if (!instance.expression_map.empty()) {
      std::fill(instance.weights.begin(), instance.weights.end(), 0.0f);
      for (u32 c = 0; c < instance.expression_map.size(); ++c) {
        i32 index = instance.expression_map[c];
        if (index >= 0) instance.weights[static_cast<u32>(index)] = expression_.channel_weight(c);
      }
    } else if (!instance.weights.empty()) {
      // No imported track (e.g. an ARKit-style blendshape face): sweep one
      // target at a time, eased in and out, so the expressions cycle live.
      std::fill(instance.weights.begin(), instance.weights.end(), 0.0f);
      const f32 period = 1.2f;  // seconds per target
      u32 index = static_cast<u32>(morph_time_ / period) % instance.weights.size();
      f32 phase = std::fmod(morph_time_, period) / period;
      instance.weights[index] = std::sin(phase * 3.14159265f);
    }
    render::DrawItem draw;
    draw.mesh = instance.mesh;
    draw.transform = instance.transform;
    draw.prev_transform = instance.transform;
    draw.morph_offset = static_cast<i32>(view.morph_weights.size());
    draw.morph_count = anim::AppendActiveMorphWeights(instance.weights, &view.morph_weights);
    if (draw.morph_count == 0) draw.morph_offset = -1;
    view.draws.push_back(draw);
  }
}

void Viewer::OnFrameEnd() {
  // --shot / --shot-frames win over RX_UI_SHOT / RX_UI_SHOT_FRAMES; the env
  // vars stay live for the capture scripts that already drive them.
  const char* shot = !config_.shot_path.empty() ? config_.shot_path.c_str() : UiShot.get();
  if (shot) {
    static int ui_shot_frames = 0;
    static const int ui_shot_target = [this] {
      if (config_.shot_frames > 0) return config_.shot_frames;
      return UiShotFrames.get() > 0 ? UiShotFrames.get() : 30;
    }();
    ++ui_shot_frames;
    if (bool(UiShotSeq)) {
      // Sequence capture: request a numbered frame each OnFrameEnd (each is
      // written by the next RenderFrame), then quit once the last one landed.
      if (ui_shot_frames <= ui_shot_target) {
        char path[512];
        std::snprintf(path, sizeof(path), "%s_%04d.png", shot, ui_shot_frames);
        renderer_->CaptureScreenshot(path);
      } else {
        host_->RequestQuit();
      }
      return;
    }
    // CaptureScreenshot is deferred: it is written by the NEXT RenderFrame.
    // Request at the target frame, then quit one frame later so the write
    // actually lands.
    if (ui_shot_frames == ui_shot_target) {
      renderer_->CaptureScreenshot(shot);
      // Perf breadcrumb for headless A/B runs alongside the capture.
      RX_INFO("gpu frame at capture: {:.2f} ms", renderer_->gpu_frame_ms());
    } else if (ui_shot_frames > ui_shot_target) {
      host_->RequestQuit();
    }
  }
}

void Viewer::OnShutdown() {
  // Close the endpoint while the world it dispatches into is still standing, so
  // the socket file goes away at the moment the engine stops serving it rather
  // than whenever the viewer is destroyed.
  authoring_endpoint_.Stop();
  bridge_.reset();
  // Release demo GPU resources (scenehook raw pipelines) before the host tears
  // the renderer's device down.
  if (tattoo_receiver_ != 0 && renderer_) {
    renderer_->ReleaseDecalReceiver(tattoo_receiver_);
    tattoo_receiver_ = 0;
  }
  // Before the host tears the ecs::World down: the streamer owns every entity
  // its cells materialized and has to give them back while there is a world to
  // give them back to.
  world_stream_.Shutdown();
  if (demos_) demos_->Shutdown();
  if (window_) debug_ui_.Shutdown();
}

}  // namespace rx
