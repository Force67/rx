#include "viewer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <base/option.h>

#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "scene/components.h"

#include "demo_scenes.h"

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
// driving the app.
base::Option<const char*> UiShot{"ui.shot", nullptr, "RX_UI_SHOT"};
base::Option<int> UiShotFrames{"ui.shot.frames", 30, "RX_UI_SHOT_FRAMES"};
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

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, *renderer_)) {
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
  ctx_.actions = actions_;
  demos_ = std::make_unique<DemoScenes>(ctx_);

  if (physics_->initialized()) CreatePhysicsCubeAsset();

  if (!config_.gltf_path.empty()) {
    if (!LoadGltfScene()) return false;
  } else {
    demos_->CreateDemoScene();
  }

  return true;
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

bool Viewer::LoadGltfScene() {
  asset::GltfScene scene;
  if (!asset::LoadGltfScene(config_.gltf_path, &scene)) return false;

  if (!config_.headless) {
    for (const asset::Texture& texture : scene.textures) {
      if (texture.id) renderer_->UploadTexture(texture);
    }
    for (const asset::Material& material : scene.materials) renderer_->UploadMaterial(material);
    for (const asset::Mesh& mesh : scene.meshes) renderer_->UploadMesh(mesh);
  }

  for (const asset::GltfScene::Instance& instance : scene.instances) {
    ecs::Entity entity = world_->Create();
    scene::Transform transform;
    transform.position[0] = instance.position.x;
    transform.position[1] = instance.position.y;
    transform.position[2] = instance.position.z;
    std::memcpy(transform.rotation, instance.rotation, sizeof(transform.rotation));
    transform.scale = instance.scale;
    world_->Add(entity, transform);
    world_->Add(entity, scene::Renderable{scene.meshes[instance.mesh_index].id});
  }

  // Sponza-friendly start: inside the atrium looking down the long axis.
  camera_.set_position({-7.0f, 1.7f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, 0.0f);
  camera_.speed = 4.0f;
  return true;
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
}

void Viewer::OnUpdate(f32 frame_delta) {
  DriveSunFromClock();
  debug_ui_.BeginFrame();
  UpdateCamera(frame_delta);
}

void Viewer::OnBuildView(f32 frame_delta, render::FrameView& view) {
  view.camera.eye = camera_.position();
  view.camera.target = camera_.target();
  demos_->EmitToView(frame_delta, view);
  debug_ui_.Build(*renderer_, camera_, frame_delta, &view);
}

void Viewer::OnFrameEnd() {
  if (const char* shot = UiShot.get()) {
    static int ui_shot_frames = 0;
    static const int ui_shot_target = [] {
      return UiShotFrames.get() > 0 ? UiShotFrames.get() : 30;
    }();
    ++ui_shot_frames;
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
  if (!config_.headless) debug_ui_.Shutdown();
}

}  // namespace rx
