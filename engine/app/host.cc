#include "app/host.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include <base/option.h>

#include "core/feature_registry.h"
#include "core/log.h"
#include "core/math.h"
#include "scene/components.h"

// Host lifecycle and the per-frame heartbeat: subsystem bringup in dependency
// order, the resolved render preset, the fixed-step simulation loop and the
// render path that gathers entity draws into the FrameView and submits it.
// Everything game-specific happens inside the Application callbacks.
namespace rx::app {
namespace {
// Host-startup options. Namespace scope, so they register before
// InitOptionsFromEnv() runs. WinW/WinH=0 keep the WindowDesc default
// (1920x1080); they shrink the window for fast headless capture (e.g. the
// software-rendered swrun path).
base::Option<int> WinW{"win.width", 0, "RX_WIN_W"};
base::Option<int> WinH{"win.height", 0, "RX_WIN_H"};
base::Option<bool> NoOcclusion{"no.occlusion", false, "RX_NO_OCCLUSION"};
// Timescale (0 freezes time) overrides the default day/night speed; GameHour
// overrides the mid-morning start the world boots lit at.
base::Option<float> Timescale{"timescale", 0.0f, "RX_TIMESCALE"};
base::Option<float> GameHour{"game.hour", 11.0f, "RX_GAME_HOUR"};
// RX_FIXED_DT=<seconds> locks every frame to one delta (frame-index-pure
// animation for golden-image captures; wall clock stops mattering).
base::Option<float> FixedDt{"fixed.dt", 0.0f, "RX_FIXED_DT"};
}  // namespace

bool Host::Initialize(const AppConfig& config, Application& app,
                      std::unique_ptr<Window> window) {
  config_ = config;
  app_ = &app;
  InitFeatures();              // apply RX_FEATURES overrides before any flag read
  base::InitOptionsFromEnv();  // populate every base::Option from the environment
  jobs_ = std::make_unique<JobSystem>();
  ConfigureClock(20.0f);

  if (!config_.headless) {
    WindowDesc desc;
    if (WinW > 0) desc.width = static_cast<u32>(WinW.get());
    if (WinH > 0) desc.height = static_cast<u32>(WinH.get());
    window_ = window ? std::move(window) : Window::Create(desc);
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
    ApplyRenderPreset();
  }

  // Audio comes up before content loads (it reads sound bytes lazily through
  // the Vfs). Headless runs and mute (RX_AUDIO_MUTE) open no device and run
  // silent; the rest of the engine is unaffected either way.
  audio_ = std::make_unique<audio::AudioSystem>();
  audio_->Initialize(&vfs_);

  if (physics_.Initialize()) {
    // Mirror enrolled dynamic bodies back into their ECS transforms after
    // each step, so renderable entities follow their physics proxies.
    scheduler_.AddSystem(ecs::Stage::kSim, "physics", [this](ecs::World& world, f32 dt) {
      physics_.Update(dt);
      for (const PhysicsBinding& body : physics_bindings_) {
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

  // Hand the application every service it may wire against. Addresses are
  // host members, stable until Shutdown.
  services_.host = this;
  services_.window = window_.get();
  services_.jobs = jobs_.get();
  services_.clock = &clock_;
  services_.world = &world_;
  services_.scheduler = &scheduler_;
  services_.renderer = &renderer_;
  services_.physics = &physics_;
  services_.vfs = &vfs_;
  services_.audio = audio_.get();
  services_.input_map = &input_map_;
  services_.actions = &actions_;
  services_.physics_bindings = &physics_bindings_;
  services_.hair_bindings = &hair_bindings_;

  return app_->OnInitialize(services_);
}

void Host::ApplyRenderPreset() {
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
  tuned.rcgi = env.rcgi;            // honor RX_RCGI over the preset
  tuned.rcgi_intensity = env.rcgi_intensity;
  tuned.sdf = env.sdf;              // honor RX_SDF over the preset
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

void Host::ConfigureClock(f32 base_timescale) {
  f32 timescale = base_timescale > 0 ? base_timescale : 20.0f;
  if (Timescale.overridden() && Timescale.get() >= 0) timescale = Timescale.get();
  const f32 start_hour = GameHour.get();
  clock_.Configure(start_hour, timescale);
  RX_INFO("day/night clock: start hour {:.1f}, timescale {:.0f}", start_hour, timescale);
}

bool Host::RunFrame() {
  if (quit_.load(std::memory_order_relaxed)) return false;
  if (FixedDt.get() > 0.0f) timer_.set_fixed_delta(static_cast<f64>(FixedDt.get()));
  if (window_ && !window_->PumpEvents()) return false;
  // Resolve this pump's raw keyboard/mouse + gamepad state into semantic
  // actions for the application to read.
  if (window_) input_map_.Resolve(window_->input(), window_->gamepad(), &actions_);

  int steps = timer_.Tick();
  f32 dt = static_cast<f32>(timer_.fixed_step());
  f32 frame_delta = static_cast<f32>(timer_.frame_delta());
  // Advance the day/night clock by the real frame time; applications derive
  // sun/sky from it.
  clock_.Advance(timer_.frame_delta());
  for (int i = 0; i < steps; ++i) {
    scheduler_.RunStage(ecs::Stage::kPreSim, world_, dt);
    scheduler_.RunStage(ecs::Stage::kSim, world_, dt);
    scheduler_.RunStage(ecs::Stage::kPostSim, world_, dt);
    app_->OnFixedStep(dt);
  }

  // Frame-cadence simulation, run in both windowed and headless modes so a
  // dedicated server advances the same authoritative logic a client does.
  app_->OnSimulate(frame_delta);

  if (!config_.headless) {
    app_->OnUpdate(frame_delta);

    // Mirror enrolled strand grooms into their renderer hair grooms: read the
    // simulated node positions back and feed the ribbon draw.
    for (const HairStrandBinding& hair : hair_bindings_) {
      u32 count = physics_.StrandGroomPositionCount(hair.strands);
      if (count == 0) continue;
      hair_positions_.resize(count);
      if (physics_.GetStrandGroomPositions(hair.strands, hair_positions_.data(), count)) {
        renderer_.SetHairGroomPoints(hair.groom, hair_positions_.data(), count);
      }
    }

    scheduler_.RunStage(ecs::Stage::kPreRender, world_, frame_delta);

    render::FrameView view;
    view.frame_delta_seconds = frame_delta;
    if (config_.gather_entity_draws) GatherEntityDraws(view);
    app_->OnBuildView(frame_delta, view);
    // Move the audio listener to this frame's viewpoint, so positional
    // voices pan and attenuate around the camera.
    if (audio_) {
      const Vec3 eye = view.camera.eye;
      const Vec3 forward = Normalize(view.camera.target - eye);
      audio_->SetListener(eye, forward, Vec3{0, 1, 0});
    }
    renderer_.RenderFrame(view);
    app_->OnFrameEnd();
  } else {
    // No vsync to pace the loop; yield between fixed steps instead of
    // spinning a core.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return !quit_.load(std::memory_order_relaxed);
}

namespace {

Mat4 LocalTransformMatrix(const scene::Transform& t) {
  return MakeTranslation({t.position[0], t.position[1], t.position[2]}) *
         MakeFromQuat(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]) *
         MakeScale(t.scale);
}

}  // namespace

void Host::GatherEntityDraws(render::FrameView& view) {
  // Rebuilt every frame so destroyed entities drop out on their own.
  base::UnorderedMap<u64, Mat4> transforms;

  // Hierarchy is opt-in per world. A world with zero Parent components visits no
  // Parent archetype here, so this probe is near-free and the parent-free path
  // below stays byte-identical to the pre-hierarchy behavior.
  bool any_parent = false;
  world_.Each<scene::Parent>([&](ecs::Entity, scene::Parent&) { any_parent = true; });

  auto emit = [&](ecs::Entity entity, const Mat4& current, const asset::AssetId& mesh) {
    u64 key = static_cast<u64>(entity.generation) << 32 | entity.index;
    const Mat4* prev = prev_transforms_.find(key);
    render::DrawItem item{mesh.hash, current, prev ? *prev : current};
    if (const auto* tint = world_.Get<scene::Tint>(entity)) item.tint = tint->rgb;
    view.draws.push_back(item);
    transforms.insert(key, current);
  };

  if (!any_parent) {
    world_.Each<scene::Transform, scene::Renderable>(
        [&](ecs::Entity entity, scene::Transform& transform, scene::Renderable& renderable) {
          if (world_.Has<scene::Hidden>(entity)) return;
          emit(entity, LocalTransformMatrix(transform), renderable.mesh);
        });
  } else {
    // Compose the world matrix up the Parent chain; an entity without a Parent
    // yields its own local matrix, exactly as the fast path above.
    auto world_matrix = [&](ecs::Entity e) -> Mat4 {
      scene::Transform* t = world_.Get<scene::Transform>(e);
      Mat4 m = t ? LocalTransformMatrix(*t) : Mat4::Identity();
      scene::Parent* p = world_.Get<scene::Parent>(e);
      int guard = 0;  // bound a corrupt cycle
      while (p && p->value && world_.IsAlive(p->value) && guard++ < 4096) {
        ecs::Entity pe = p->value;
        scene::Transform* pt = world_.Get<scene::Transform>(pe);
        m = (pt ? LocalTransformMatrix(*pt) : Mat4::Identity()) * m;
        p = world_.Get<scene::Parent>(pe);
      }
      return m;
    };
    world_.Each<scene::Transform, scene::Renderable>(
        [&](ecs::Entity entity, scene::Transform&, scene::Renderable& renderable) {
          if (world_.Has<scene::Hidden>(entity)) return;
          emit(entity, world_matrix(entity), renderable.mesh);
        });
  }
  prev_transforms_ = std::move(transforms);
}

int Host::Run() {
  while (RunFrame()) {
  }
  return 0;
}

void Host::OnSurfaceDestroyed() {
  if (!config_.headless) renderer_.DestroySurface();
}

void Host::OnSurfaceCreated() {
  if (!config_.headless) renderer_.RecreateSurface();
}

Host::~Host() { Shutdown(); }

void Host::Shutdown() {
  if (shut_down_) return;  // idempotent: explicit Shutdown then destructor
  shut_down_ = true;
  // Stop the audio device thread early, before the systems whose sounds it
  // might still be streaming go away.
  if (audio_) audio_->Shutdown();
  if (!config_.headless) renderer_.WaitIdle();
  // The renderer is idle but alive: the application drops its GPU-dependent
  // resources (UI backends etc.) before the device goes away.
  if (app_) app_->OnShutdown();
  if (!config_.headless) renderer_.Shutdown();
  if (jobs_) jobs_->WaitIdle();
}

}  // namespace rx::app
