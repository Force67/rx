#include "engine.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <base/option.h>

#include "asset/asset_database.h"
#include "asset/gltf_loader.h"
#include "asset/primitives.h"
#include "core/feature_registry.h"
#include "core/log.h"
#include "scene/components.h"

// Engine lifecycle and service wiring: construction (renderer/UI/physics
// bringup and the shared EngineContext), the resolved render preset, the
// front-door content dispatch, and ordered teardown. The frame loop and
// camera/input live in sibling engine translation units (see frame_loop.cc,
// camera_input.cc).
namespace rx {
namespace {
// Engine-startup options. Namespace scope, so they register before
// InitOptionsFromEnv() runs. WinW/WinH=0 keep the WindowDesc default
// (1920x1080); they shrink the window for fast headless capture (e.g. the
// software-rendered swrun path).
base::Option<int> WinW{"win.width", 0, "RX_WIN_W"};
base::Option<int> WinH{"win.height", 0, "RX_WIN_H"};
// SunDir pins a fixed sun for headless lighting/shadow tests (its presence
// disables the world clock driving the sun); the renderer parses the value.
base::Option<const char*> SunDir{"sun.dir", nullptr, "RX_SUN_DIR"};
base::Option<bool> NoOcclusion{"no.occlusion", false, "RX_NO_OCCLUSION"};
// Timescale (0 freezes time) overrides the default day/night speed; GameHour
// overrides the mid-morning start the world boots lit at.
base::Option<float> Timescale{"timescale", 0.0f, "RX_TIMESCALE"};
base::Option<float> GameHour{"game.hour", 11.0f, "RX_GAME_HOUR"};
}  // namespace

bool Engine::Initialize(const EngineConfig& config, std::unique_ptr<Window> window) {
  config_ = config;
  InitFeatures();              // apply RX_FEATURES overrides before any flag read
  base::InitOptionsFromEnv();  // populate every base::Option from the environment
  jobs_ = std::make_unique<JobSystem>();
  // When SunDir is set the world clock stops driving the day/night cycle.
  // Seed the clock now so the demo and glTF scenes get a lit time of day.
  drive_sun_from_clock_ = SunDir.get() == nullptr;
  ConfigureClock(20.0f);

  if (!config_.headless) {
    WindowDesc desc;
    if (WinW > 0) desc.width = static_cast<u32>(WinW.get());
    if (WinH > 0) desc.height = static_cast<u32>(WinH.get());
    window_ = window ? std::move(window) : Window::Create(desc);
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
    ApplyRenderPreset();
  }

  if (!config_.headless) {
    if (!debug_ui_.Initialize(*window_, renderer_)) {
      RX_WARN("debug ui unavailable");
    }
    debug_ui_.set_clock(&clock_);  // Lighting panel scrubs the day/night cycle
  }

  // Wire the shared service bundle and build the demo subsystem. The late-built
  // services (assets) are filled into the context as they come up.
  ctx_.config = &config_;
  ctx_.world = &world_;
  ctx_.scheduler = &scheduler_;
  ctx_.renderer = &renderer_;
  ctx_.camera = &camera_;
  ctx_.physics = &physics_;
  ctx_.vfs = &vfs_;
  ctx_.debug_ui = &debug_ui_;
  ctx_.physics_entities = &physics_entities_;
  // Audio comes up before content loads (it reads sound bytes lazily through
  // the Vfs). Headless runs and mute (RX_AUDIO_MUTE) open no device and run
  // silent; the rest of the engine is unaffected either way.
  audio_ = std::make_unique<audio::AudioSystem>();
  audio_->Initialize(&vfs_);
  ctx_.audio = audio_.get();
  demos_ = std::make_unique<DemoScenes>(ctx_);

  if (physics_.Initialize()) {
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
      renderer_.UploadMaterial(wood);
      renderer_.UploadMesh(cube);
    }
    scheduler_.AddSystem(ecs::Stage::kSim, "physics", [this](ecs::World& world, f32 dt) {
      physics_.Update(dt);
      for (const PhysicsEntity& body : physics_entities_) {
        scene::Transform* transform = world.Get<scene::Transform>(body.entity);
        if (!transform) continue;
        Vec3 position;
        f32 rotation[4];
        if (physics_.GetBodyTransform(body.body, &position, rotation)) {
          transform->position[0] = position.x;
          transform->position[1] = position.y;
          transform->position[2] = position.z;
          std::memcpy(transform->rotation, rotation, sizeof(rotation));
        }
      }
    });
  }

  if (!config_.gltf_path.empty()) {
    if (!LoadGltfScene()) return false;
  } else {
    demos_->CreateDemoScene();
  }

  return true;
}

bool Engine::LoadGltfScene() {
  asset::GltfScene scene;
  if (!asset::LoadGltfScene(config_.gltf_path, &scene)) return false;

  if (!config_.headless) {
    for (const asset::Texture& texture : scene.textures) {
      if (texture.id) renderer_.UploadTexture(texture);
    }
    for (const asset::Material& material : scene.materials) renderer_.UploadMaterial(material);
    for (const asset::Mesh& mesh : scene.meshes) renderer_.UploadMesh(mesh);
  }

  for (const asset::GltfScene::Instance& instance : scene.instances) {
    ecs::Entity entity = world_.Create();
    scene::Transform transform;
    transform.position[0] = instance.position.x;
    transform.position[1] = instance.position.y;
    transform.position[2] = instance.position.z;
    std::memcpy(transform.rotation, instance.rotation, sizeof(transform.rotation));
    transform.scale = instance.scale;
    world_.Add(entity, transform);
    world_.Add(entity, scene::Renderable{scene.meshes[instance.mesh_index].id});
  }

  // Sponza-friendly start: inside the atrium looking down the long axis.
  camera_.set_position({-7.0f, 1.7f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, 0.0f);
  camera_.speed = 4.0f;
  return true;
}

void Engine::ApplyRenderPreset() {
  render::Device* device = renderer_.device();
  if (!device || device->is_stub()) return;  // no gpu, nothing to tune
  const render::DeviceCaps& caps = device->caps();
  render::QualityPreset resolved = render::ResolvePreset(config_.preset, caps);
  render::RenderSettings tuned = render::PresetSettings(resolved, caps);

  // Explicit reconstruction flags (--no-taa / --upscaler) still win over the
  // preset's choice; --no-rt already gates ray tracing at the device level.
  if (config_.renderer.aa_mode == render::AntiAliasingMode::kNone) {
    tuned.aa_mode = render::AntiAliasingMode::kNone;
    tuned.upscaler = render::UpscalerKind::kNone;
  } else if (config_.renderer.upscaler != render::UpscalerKind::kNone) {
    tuned.upscaler = config_.renderer.upscaler;
    tuned.aa_mode = render::AntiAliasingMode::kUpscaler;
  }

  // Initialize() applied the RX_DEBUG_VIEW / RX_PATHTRACE debug env overrides;
  // carry them through so a preset never silently disables headless captures.
  const render::RenderSettings& env = renderer_.settings();
  if (env.aa_mode == render::AntiAliasingMode::kMsaa) {  // honor RX_MSAA
    tuned.aa_mode = env.aa_mode;
    tuned.msaa_samples = env.msaa_samples;
    tuned.upscaler = render::UpscalerKind::kNone;
  }
  tuned.debug_view = env.debug_view;
  if (env.debug_view != render::DebugView::kOff) {
    tuned.auto_exposure = false;
    tuned.exposure = 1.0f;
  }
  if (env.path_trace) tuned.path_trace = true;
  // Carry the path-tracer mode + tunables (RX_PATHTRACE_RECON / _REFERENCE /
  // _SPP / _ACCUM ...) through the preset, or env-selected recon/reference
  // silently falls back to the NRD path.
  tuned.path_trace_reference = env.path_trace_reference;
  tuned.path_trace_recon = env.path_trace_recon;
  tuned.path_trace_spp = env.path_trace_spp;
  tuned.path_trace_accum = env.path_trace_accum;
  tuned.path_trace_recon_weight = env.path_trace_recon_weight;
  tuned.path_trace_recon_atrous = env.path_trace_recon_atrous;
  tuned.path_trace_recon_debug = env.path_trace_recon_debug;
  tuned.path_trace_restir = env.path_trace_restir;
  tuned.path_trace_restir_di = env.path_trace_restir_di;
  tuned.hdr_output = env.hdr_output;
  tuned.hdr_paper_white = env.hdr_paper_white;
  tuned.path_trace_rr = env.path_trace_rr;
  if (env.wireframe) tuned.wireframe = true;  // honor RX_WIREFRAME over the preset
  tuned.ssr = env.ssr;                        // honor RX_SSR over the preset
  tuned.ssgi = env.ssgi;                      // honor RX_SSGI over the preset
  tuned.color_grade = env.color_grade;        // presets never set a grade
  tuned.sun_direction = env.sun_direction;    // honor RX_SUN_DIR over the default
  // Sky/weather env overrides (RX_AERIAL / RX_CLOUDS / RX_CLOUD_COVERAGE /
  // RX_PRECIP / RX_SNOW), so they survive the preset.
  tuned.fog = env.fog;  // honor RX_FOG over the preset (fog params are defaults)
  tuned.motion_blur = env.motion_blur;  // honor RX_MOTION_BLUR over the preset
  tuned.lens_flare = env.lens_flare;    // honor RX_LENS_FLARE over the preset
  tuned.film_grain = env.film_grain;    // honor RX_FILM_GRAIN over the preset
  tuned.dof = env.dof;
  tuned.dof_focus = env.dof_focus;
  tuned.dof_aperture = env.dof_aperture;
  tuned.sss = env.sss;  // honor RX_SSS over the preset
  tuned.sss_width = env.sss_width;
  tuned.async_compute = env.async_compute;  // honor RX_ASYNC_COMPUTE
  tuned.frame_generation = env.frame_generation;  // honor RX_FRAMEGEN
  tuned.local_shadows = env.local_shadows;  // honor RX_LOCAL_SHADOWS
  tuned.froxel_fog = env.froxel_fog;  // honor RX_FROXEL
  tuned.froxel_density = env.froxel_density;
  tuned.vrs = env.vrs;  // honor RX_VRS
  tuned.texture_budget_mb = env.texture_budget_mb;  // honor RX_TEX_BUDGET_MB
  tuned.gpu_pass_timings = env.gpu_pass_timings;    // honor RX_GPU_TIMINGS
  tuned.dynamic_resolution = env.dynamic_resolution;  // honor RX_DRS
  tuned.dynamic_target_ms = env.dynamic_target_ms;
  tuned.dynamic_min_scale = env.dynamic_min_scale;
  tuned.restir_di = env.restir_di;  // honor RX_RESTIR_DI
  tuned.fft_ocean = env.fft_ocean;  // honor RX_FFT_OCEAN
  tuned.vrs_threshold = env.vrs_threshold;
  tuned.aerial_perspective = env.aerial_perspective;
  tuned.clouds = env.clouds;
  tuned.cloud_coverage = env.cloud_coverage;
  tuned.precipitation = env.precipitation;
  tuned.precip_snow = env.precip_snow;
  tuned.aurora = env.aurora;
  if (NoOcclusion) tuned.gpu_occlusion = false;  // a/b baseline

  renderer_.settings() = tuned;
  RX_INFO("render preset: {} ({})", render::PresetName(resolved),
          config_.preset == render::QualityPreset::kAuto ? "auto" : "forced");
}

void Engine::ConfigureClock(f32 base_timescale) {
  f32 timescale = base_timescale > 0 ? base_timescale : 20.0f;
  if (Timescale.overridden() && Timescale.get() >= 0) timescale = Timescale.get();
  const f32 start_hour = GameHour.get();
  clock_.Configure(start_hour, timescale);
  RX_INFO("day/night clock: start hour {:.1f}, timescale {:.0f}", start_hour, timescale);
}

int Engine::Run() {
  while (RunFrame()) {
  }
  return 0;
}

void Engine::OnSurfaceDestroyed() {
  if (!config_.headless) renderer_.DestroySurface();
}

void Engine::OnSurfaceCreated() {
  if (!config_.headless) renderer_.RecreateSurface();
}

Engine::~Engine() { Shutdown(); }

void Engine::Shutdown() {
  if (shut_down_) return;  // idempotent: explicit Shutdown then destructor
  shut_down_ = true;
  // Stop the audio device thread early, before the systems whose sounds it
  // might still be streaming go away.
  if (audio_) audio_->Shutdown();
  if (!config_.headless) {
    renderer_.WaitIdle();
    debug_ui_.Shutdown();
    renderer_.Shutdown();
  }
  if (jobs_) jobs_->WaitIdle();
}

}  // namespace rx
