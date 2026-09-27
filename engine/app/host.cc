#include "app/host.h"

#include <string.h>

#include <base/option.h>

#include "app/platform_config.h"
#include "base/check.h"
#include "asset/content_mounts.h"
#include "base/algorithm.h"
#include "base/atomic.h"
#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/threading/thread.h"
#include <base/hashing/cuid2.h>

#include "core/app_identity.h"
#include "core/feature_registry.h"
#include "core/log.h"
#include "core/math.h"
#include "core/memory/frame_arena.h"
#include "core/memory/memory_config.h"
#include "core/platform.h"
#include "core/sort.h"
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
// Unset means fullscreen on a Deck and under gamescope, windowed elsewhere: the
// default 1920x1080 window is larger than the Deck's 1280x800 panel, and
// gamescope scales whatever it gets to cover its output anyway.
base::Option<bool> Fullscreen{"win.fullscreen", false, "RX_FULLSCREEN"};
// RX_FRAME_STATS=<seconds> logs the frame-time spread over each window: the
// average alone hides the hitches a handheld player feels.
base::Option<float> FrameStats{"frame.stats", 0.0f, "RX_FRAME_STATS"};
// Frame-rate cap while the window has lost focus (Steam menu open on a Deck,
// alt-tabbed on a desktop): nobody is watching, so spend no power on it.
// 0 renders at full rate regardless.
base::Option<int> UnfocusedFps{"unfocused.fps", 10, "RX_UNFOCUSED_FPS"};
// Touch doubling as the mouse is the SDL default and keeps mouse-only UI usable
// under a finger. Handhelds turn it off: with mouse look in relative mode a
// thumb resting on the panel drags the camera.
base::Option<bool> TouchMouse{"touch.emits_mouse", true, "RX_TOUCH_MOUSE"};
// Timescale (0 freezes time) overrides the default day/night speed; GameHour
// overrides the mid-morning start the world boots lit at.
base::Option<float> Timescale{"timescale", 0.0f, "RX_TIMESCALE"};
base::Option<float> GameHour{"game.hour", 11.0f, "RX_GAME_HOUR"};
// RX_FIXED_DT=<seconds> locks every frame to one delta (frame-index-pure
// animation for golden-image captures; wall clock stops mattering).
base::Option<float> FixedDt{"fixed.dt", 0.0f, "RX_FIXED_DT"};
// The rx splash plate. RX_SPLASH=0 suppresses it; RX_SPLASH=1 is also the only
// way to get it into a capture, which WantsSplash otherwise refuses.
base::Option<bool> ShowSplash{"splash", true, "RX_SPLASH"};
base::Option<float> SplashSeconds{"splash.seconds", ui::Splash::kDefaultSeconds,
                                  "RX_SPLASH_SECONDS"};
void ApplyMemoryPlan(const base::String& text) {
  mem::MemoryConfig plan;
  mem::ParseMemoryConfigText(text, plan);
  mem::ApplyMemoryConfig(plan);
}

}  // namespace

bool Host::Initialize(const AppConfig& config, Application& app,
                      base::UniquePointer<Window> window) {
  config_ = config;
  app_ = &app;
  InitFeatures();              // apply RX_FEATURES overrides before any flag read
  base::InitOptionsFromEnv();  // populate every base::Option from the environment
  // Identity first: the mounts, the per-user folders and the window all name
  // the app by it.
  BASE_FATAL_CHECK(base::IsValidCuid2(config_.id.c_str()),
                   "AppConfig::id must be a cuid2: run `rx --new-app-id` for one");
  BASE_FATAL_CHECK(IsValidAppName(config_.name),
                   "AppConfig::name must be a slug: lowercase [a-z0-9_-], not rxe or user");
  BASE_FATAL_CHECK(!config_.title.empty(), "AppConfig::title must be set");
  SetAppIdentity({config_.id, config_.name, config_.title});
  // The engine's and the game's content (rxe://, <title>://) mount first: the
  // platform config lives there, and the application mounts over it later.
  asset::MountContent(vfs_, config_.name);
  // What the config can say before the gpu picks a tier: the default.ini
  // files' options (window, fullscreen...) and memory plan, in place before any
  // subsystem starts allocating in earnest.
  PlatformConfig startup;
  ReadPlatformChain(vfs_, config_.name, render::QualityPreset::kAuto, &startup);
  ApplyPlatformOptions(startup);
  ApplyMemoryPlan(startup.memory);
  jobs_ = base::MakeUnique<JobSystem>();
  ConfigureClock(20.0f);
  // An app that asked for lockstep (AppConfig::fixed_delta, i.e. a capture run)
  // gets it unless the caller spoke about the clock themselves: RX_FIXED_DT set
  // to 0 is how you ask a capture for real wall-clock timing back, and a
  // nonzero one is re-applied every RunFrame below.
  if (config_.fixed_delta > 0.0f && !FixedDt.overridden())
    timer_.set_fixed_delta(static_cast<f64>(config_.fixed_delta));

  // --width/--height first, then RX_WIN_W/RX_WIN_H, then the WindowDesc
  // default; the same size answers for a window and for an offscreen target.
  WindowDesc desc;
  desc.title = GetAppIdentity().title;
  if (WinW > 0) desc.width = static_cast<u32>(WinW.get());
  if (WinH > 0) desc.height = static_cast<u32>(WinH.get());
  if (config_.width > 0) desc.width = config_.width;
  if (config_.height > 0) desc.height = config_.height;
  if (!config_.headless) {
    desc.touch_emits_mouse = TouchMouse;
    if (Fullscreen.overridden())
      desc.fullscreen = Fullscreen;
    else if (WinW <= 0 && config_.width == 0)
      desc.fullscreen = IsSteamDeck() || IsGamescope();
    window_ = window ? base::move(window) : Window::Create(desc);
    if (!renderer_.Initialize(config_.renderer, *window_)) return false;
    ApplyRenderPreset();
  } else if (config_.offscreen) {
    if (!renderer_.InitializeOffscreen(config_.renderer, desc.width, desc.height))
      return false;
    ApplyRenderPreset();
  }

  // Audio comes up before content loads (it reads sound bytes lazily through
  // the Vfs). Headless runs and mute (RX_AUDIO_MUTE) open no device and run
  // silent; the rest of the engine is unaffected either way.
  audio_ = base::MakeUnique<audio::AudioSystem>();
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
          base::MemCopy(transform->rotation, rotation, sizeof(rotation));
        }
      }
    });
  }

  // Hand the application every service it may wire against. Addresses are
  // host members, stable until Shutdown.
  services_.host = this;
  services_.window = window_.Get_UseOnlyIfYouKnowWhatYouareDoing();
  services_.jobs = jobs_.Get_UseOnlyIfYouKnowWhatYouareDoing();
  services_.clock = &clock_;
  services_.world = &world_;
  services_.scheduler = &scheduler_;
  services_.renderer = &renderer_;
  services_.physics = &physics_;
  services_.vfs = &vfs_;
  services_.audio = audio_.Get_UseOnlyIfYouKnowWhatYouareDoing();
  services_.input_map = &input_map_;
  services_.actions = &actions_;
  services_.physics_bindings = &physics_bindings_;
  services_.hair_bindings = &hair_bindings_;

  // Before OnInitialize, so a splash that cannot come up says so before the
  // application spends seconds loading content, and so the ultragui registry
  // the plate parks (ui::Splash::Initialize) is the one the application is
  // about to build its own UI into.
  if (WantsSplash()) {
    auto splash = base::MakeUnique<ui::Splash>();
    if (splash->Initialize(*window_, renderer_, vfs_, SplashSeconds.get()))
      splash_ = base::move(splash);
  }

  return app_->OnInitialize(services_);
}

bool Host::WantsSplash() const {
  if (!config_.splash || !ShowSplash) return false;
  if (config_.headless || !window_) return false;
  // A lockstep run exists to write a png someone will compare, and the plate
  // covers the whole screen: leaving it on would land it in every capture that
  // renders fewer than ~140 frames. Setting RX_SPLASH by hand is how you ask
  // for a capture of the plate itself.
  if ((timer_.fixed_delta() > 0.0 || FixedDt.get() > 0.0f) && !ShowSplash.overridden())
    return false;
  return true;
}

void Host::ApplyRenderPreset() {
  render::Device* device = renderer_.device();
  if (!device || device->is_stub()) return;  // no gpu, nothing to tune
  const render::DeviceCaps& caps = device->caps();
  render::QualityPreset resolved = render::ResolvePreset(config_.preset, caps);
  // The whole platform config for the tier: engine then game, default.ini then
  // <tier>.ini. Options and the memory plan apply again, now with the tier's.
  PlatformConfig platform;
  BASE_FATAL_CHECK(ReadPlatformChain(vfs_, config_.name, resolved, &platform),
                   "no engine platform config for the quality tier");
  ApplyPlatformOptions(platform);
  ApplyMemoryPlan(platform.memory);
  render::RenderSettings tuned = render::PresetSettings(platform.render, caps);

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
  if (env.wireframe) tuned.wireframe = true;  // honor RX_WIREFRAME over the preset

  // Every RX_* knob Renderer::Initialize read: the env wins only where it says
  // something other than the default. An unset option holds the default, and
  // copying that over would undo whatever the tier's ini chose for the field.
  const render::RenderSettings defaults;
  auto carry = [&](auto render::RenderSettings::*field) {
    if (env.*field != defaults.*field) tuned.*field = env.*field;
  };
  using RS = render::RenderSettings;
  // Path-tracer mode + tunables (RX_PATHTRACE_RECON / _REFERENCE / _SPP / ...),
  // or env-selected recon/reference silently falls back to the NRD path.
  carry(&RS::path_trace_reference);
  carry(&RS::path_trace_recon);
  carry(&RS::path_trace_spp);
  carry(&RS::path_trace_accum);
  carry(&RS::path_trace_recon_weight);
  carry(&RS::path_trace_recon_atrous);
  carry(&RS::path_trace_recon_debug);
  carry(&RS::path_trace_restir);
  carry(&RS::path_trace_restir_di);
  carry(&RS::path_trace_rr);
  carry(&RS::hdr_output);
  carry(&RS::hdr_paper_white);
  carry(&RS::ssr);
  carry(&RS::ssgi);
  carry(&RS::distance_lod);
  carry(&RS::mesh_shader_lod);
  carry(&RS::fog);
  carry(&RS::motion_blur);
  carry(&RS::lens_flare);
  carry(&RS::film_grain);
  carry(&RS::dof);
  carry(&RS::dof_focus);
  carry(&RS::dof_aperture);
  carry(&RS::sss);
  carry(&RS::sss_width);
  carry(&RS::async_compute);
  carry(&RS::frame_generation);
  carry(&RS::local_shadows);
  carry(&RS::froxel_fog);
  carry(&RS::froxel_density);
  carry(&RS::froxel_start_distance);
  carry(&RS::vrs);
  carry(&RS::vrs_threshold);
  carry(&RS::texture_budget_mb);
  carry(&RS::gpu_pass_timings);
  carry(&RS::dynamic_resolution);
  carry(&RS::dynamic_target_ms);
  carry(&RS::dynamic_min_scale);
  carry(&RS::restir_di);
  carry(&RS::rcgi_intensity);
  carry(&RS::fft_ocean);
  carry(&RS::adaptive_water);
  carry(&RS::water_field);
  carry(&RS::water_interaction);
  carry(&RS::shore_wetting);
  carry(&RS::water_caustics);
  carry(&RS::procedural_grass);
  carry(&RS::aerial_perspective);
  carry(&RS::clouds);
  carry(&RS::cloudscape);
  carry(&RS::cloudscape_steps);
  carry(&RS::cloud_coverage);
  // RX_RCGI wins in both directions, but only when explicitly set.
  if (renderer_.rcgi_env_overridden()) tuned.rcgi = env.rcgi;
  // SDF software-trace availability is a startup decision on Renderer::sdf_available_,
  // not a RenderSettings field, so it survives this wholesale preset replacement
  // with no carry needed (see RendererDesc::software_gi / settings.h note).
  // Live state no tier sets: always the renderer's.
  tuned.color_grade = env.color_grade;
  tuned.sun_direction = env.sun_direction;  // honor RX_SUN_DIR over the default
  tuned.cloudscape_controls = env.cloudscape_controls;
  tuned.weather = env.weather;
  if (NoOcclusion) tuned.gpu_occlusion = false;  // a/b baseline

  // The app profile runs last, after the tier and every env carry-through, so
  // nothing above can silently undo it.
  if (config_.tune_settings) config_.tune_settings(tuned);

  renderer_.settings() = tuned;
  RX_INFO("render preset: {} ({}), platform config with {} problem(s)",
          render::PresetName(resolved),
          config_.preset == render::QualityPreset::kAuto ? "auto" : "forced", platform.problems);
}

void Host::LogFrameStats(f32 frame_delta) {
  frame_times_.push_back(frame_delta);
  frame_stats_elapsed_ += frame_delta;
  if (frame_stats_elapsed_ < FrameStats.get()) return;
  base::Vector<f32>& t = frame_times_;
  f32 sum = 0.0f;
  for (f32 dt : t) sum += dt;
  rx::StableSort(t.data(), t.data() + t.size(), [](f32 a, f32 b) { return a < b; });
  const size_t n = t.size();
  const f32 avg_ms = sum / static_cast<f32>(n) * 1000.0f;
  RX_INFO("frame stats: {:.1f} fps, avg {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms ({} frames)",
          1000.0f / avg_ms, avg_ms, t[n * 99 / 100] * 1000.0f, t[n - 1] * 1000.0f, n);
  t.clear();
  frame_stats_elapsed_ = 0.0f;
}

void Host::ConfigureClock(f32 base_timescale) {
  f32 timescale = base_timescale > 0 ? base_timescale : 20.0f;
  if (Timescale.overridden() && Timescale.get() >= 0) timescale = Timescale.get();
  const f32 start_hour = GameHour.get();
  clock_.Configure(start_hour, timescale);
  RX_INFO("day/night clock: start hour {:.1f}, timescale {:.0f}", start_hour, timescale);
}

bool Host::RunFrame() {
  if (quit_.load(base::memory_order_relaxed)) return false;
  mem::MainFrameArena().Reset();
  if (FixedDt.get() > 0.0f) timer_.set_fixed_delta(static_cast<f64>(FixedDt.get()));
  if (window_ && !window_->PumpEvents()) return false;
  // Resolve this pump's raw keyboard/mouse + gamepad state into semantic
  // actions for the application to read.
  if (window_)
    input_map_.Resolve(window_->input(), window_->gamepad(), window_->touch(), &actions_);

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

  if (rendering()) {
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

    render::FrameView& view = frame_view_;
    view.Clear();
    view.frame_delta_seconds = frame_delta;
    if (config_.gather_entity_draws) GatherEntityDraws(view);
    app_->OnBuildView(frame_delta, view);
    // After the application built its view, so the plate covers whatever the
    // application drew; it takes FrameView::hud_draw for as long as it is up.
    if (splash_) {
      if (splash_->Update(frame_delta)) {
        splash_->Draw(view);
      } else {
        // The plate's textures are still referenced by frames in flight. This
        // is the one stall the splash costs, and it lands on the frame the
        // application becomes visible, before anything is animating.
        renderer_.WaitIdle();
        splash_.Reset();
      }
    }
    // Move the audio listener to this frame's viewpoint, so positional
    // voices pan and attenuate around the camera.
    if (audio_) {
      const Vec3 eye = view.camera.eye;
      const Vec3 forward = Normalize(view.camera.target - eye);
      audio_->SetListener(eye, forward, Vec3{0, 1, 0});
    }
    renderer_.RenderFrame(view);
    app_->OnFrameEnd();
    if (FrameStats.get() > 0.0f) LogFrameStats(frame_delta);
    // Not in a lockstep capture: its frames are the output, not a display.
    if (window_ && UnfocusedFps.get() > 0 && !(timer_.fixed_delta() > 0.0)) {
      const bool focused = window_->focused();
      if (focused != was_focused_) {
        RX_INFO("window {}", focused ? "focused" : "unfocused, throttling");
        was_focused_ = focused;
      }
      // Sleeping the render time on top keeps this simple and errs slower.
      if (!focused) base::SleepForMilliseconds(1000 / UnfocusedFps.get());
    }
  } else {
    // No vsync to pace the loop; yield between fixed steps instead of
    // spinning a core.
    base::SleepForMilliseconds(1);
  }
  return !quit_.load(base::memory_order_relaxed);
}

namespace {

Mat4 LocalTransformMatrix(const scene::Transform& t) {
  return MakeTranslation({t.position[0], t.position[1], t.position[2]}) *
         MakeFromQuat(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]) *
         MakeScale(t.scale);
}

}  // namespace

void Host::GatherEntityDraws(render::FrameView& view) {
  // Rebuilt every frame so destroyed entities drop out on their own; the two
  // maps swap roles each frame so buckets are reused instead of re-allocated.
  base::UnorderedMap<u64, Mat4>& transforms = transforms_scratch_;
  transforms.clear();
  transforms.reserve(prev_transforms_.size());

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
    if (const auto* decal = world_.Get<scene::DecalReceiver>(entity))
      item.decal_receiver = decal->handle;
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
  base::Swap(prev_transforms_, transforms_scratch_);
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
  if (rendering()) renderer_.WaitIdle();
  // A run that quit inside the first seconds still owns a plate; drop it while
  // the device it uploaded through is alive.
  splash_.Reset();
  // Destroy app-provided frame callbacks while the renderer and application
  // resources they may own are still alive.
  renderer_.ClearFrameCallbacks();
  frame_view_.Clear();
  // The renderer is idle but alive: the application drops its GPU-dependent
  // resources (UI backends etc.) before the device goes away.
  if (app_) app_->OnShutdown();
  if (rendering()) renderer_.Shutdown();
  if (jobs_) jobs_->WaitIdle();
}

}  // namespace rx::app
