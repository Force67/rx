#include "viewer.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <utility>

#include <base/option.h>

#include "anim/morph.h"
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
// RX_UI_SHOT_SEQ treats RX_UI_SHOT as a prefix and dumps every frame as
// <prefix>_NNNN.png for RX_UI_SHOT_FRAMES frames (pair with RX_FIXED_DT for
// an even cadence), for assembling headless captures into video.
base::Option<bool> UiShotSeq{"ui.shot.seq", false, "RX_UI_SHOT_SEQ"};
// Capture hook: RX_MORPH_WEIGHTS="name=w,name=w" pins named morph targets to
// fixed weights on every morphed instance (unmatched names are skipped per
// mesh), instead of the imported track / scripted sweep.
base::Option<const char*> MorphWeights{"morph.weights", nullptr, "RX_MORPH_WEIGHTS"};
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
  ctx_.hair_bindings = services.hair_bindings;
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
  }
  if (!morphed_.empty()) {
    RX_INFO("gltf: {} morphed instance(s) animated by the viewer", morphed_.size());
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
  s.night = sky.night;  // the moon-lit night would otherwise read as day
}

void Viewer::OnUpdate(f32 frame_delta) {
  DriveSunFromClock();
  debug_ui_.BeginFrame();
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
  UpdateCamera(frame_delta);
}

void Viewer::OnBuildView(f32 frame_delta, render::FrameView& view) {
  view.camera.eye = camera_.position();
  view.camera.target = camera_.target();
  demos_->EmitToView(frame_delta, view);
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
  if (const char* shot = UiShot.get()) {
    static int ui_shot_frames = 0;
    static const int ui_shot_target = [] {
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
  // Release demo GPU resources (scenehook raw pipelines) before the host tears
  // the renderer's device down.
  if (demos_) demos_->Shutdown();
  if (!config_.headless) debug_ui_.Shutdown();
}

}  // namespace rx
