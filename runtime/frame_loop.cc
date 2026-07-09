#include "engine.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include <base/option.h>

#include "core/log.h"
#include "core/math.h"
#include "core/types.h"
#include "scene/components.h"

// The Engine's per-frame heartbeat: the fixed-step simulation loop plus the
// render path that builds the FrameView and submits it. Split out of the core
// lifecycle unit so the hot loop reads on its own.
namespace rx {

// Config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
static base::Option<const char*> UiShot{"ui.shot", nullptr, "RX_UI_SHOT"};
static base::Option<int> UiShotFrames{"ui.shot.frames", 30, "RX_UI_SHOT_FRAMES"};
// RX_FIXED_DT=<seconds> locks every frame to one delta (frame-index-pure
// animation for golden-image captures; wall clock stops mattering).
static base::Option<float> FixedDt{"fixed.dt", 0.0f, "RX_FIXED_DT"};

bool Engine::RunFrame() {
  if (quit_.load(std::memory_order_relaxed)) return false;
  if (FixedDt.get() > 0.0f) timer_.set_fixed_delta(static_cast<f64>(FixedDt.get()));
  if (window_ && !window_->PumpEvents()) return false;
  // Resolve this pump's raw keyboard/mouse + gamepad state into semantic
  // actions for the camera/menu code to read.
  if (window_) input_map_.Resolve(window_->input(), window_->gamepad(), &actions_);
  {
    int steps = timer_.Tick();
    f32 dt = static_cast<f32>(timer_.fixed_step());
    // Advance the day/night clock by the real frame time; the render path
    // below derives the sun and sky tint from it.
    clock_.Advance(timer_.frame_delta());
    for (int i = 0; i < steps; ++i) {
      scheduler_.RunStage(ecs::Stage::kPreSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kSim, world_, dt);
      scheduler_.RunStage(ecs::Stage::kPostSim, world_, dt);
    }

    if (!config_.headless) {
      f32 frame_delta = static_cast<f32>(timer_.frame_delta());
      // Day/night: derive the sun direction/intensity/color/ambient from the
      // clock's time of day (unless RX_SUN_DIR pinned a fixed sun). Throttled
      // to ~0.02-hour steps so the IBL environment is not rebuilt every frame.
      if (drive_sun_from_clock_ && !ctx_.scene_owns_sun) {
        const f32 hour = clock_.hour();
        if (last_sky_hour_ < -100.0f || std::abs(hour - last_sky_hour_) >= 0.02f) {
          last_sky_hour_ = hour;
          const SkyLighting sky = ComputeSkyLighting(hour);
          auto& s = renderer_.settings();
          s.sun_direction = sky.sun_direction;
          s.sun_intensity = sky.sun_intensity;
          s.sun_color = sky.sun_color;
          s.ambient = sky.ambient;
        }
      }
      debug_ui_.BeginFrame();
      UpdateCamera(frame_delta);
      scheduler_.RunStage(ecs::Stage::kPreRender, world_, frame_delta);

      render::FrameView view;
      view.camera.eye = camera_.position();
      view.camera.target = camera_.target();
      view.frame_delta_seconds = frame_delta;
      // Move the audio listener to this frame's viewpoint, so positional
      // voices pan and attenuate around the camera.
      if (audio_) {
        const Vec3 eye = view.camera.eye;
        const Vec3 forward = Normalize(view.camera.target - eye);
        audio_->SetListener(eye, forward, Vec3{0, 1, 0});
      }
      // Rebuilt every frame so destroyed entities drop out on their own.
      base::UnorderedMap<u64, Mat4> transforms;
      world_.Each<scene::Transform, scene::Renderable>(
          [&](ecs::Entity entity, scene::Transform& transform, scene::Renderable& renderable) {
            if (world_.Has<scene::Hidden>(entity)) return;
            u64 key = static_cast<u64>(entity.generation) << 32 | entity.index;
            Mat4 current =
                MakeTranslation(
                    {transform.position[0], transform.position[1], transform.position[2]}) *
                MakeFromQuat(transform.rotation[0], transform.rotation[1], transform.rotation[2],
                             transform.rotation[3]) *
                MakeScale(transform.scale);
            const Mat4* prev = prev_transforms_.find(key);
            view.draws.push_back({renderable.mesh.hash, current, prev ? *prev : current});
            transforms.insert(key, current);
          });
      prev_transforms_ = std::move(transforms);
      demos_->EmitToView(frame_delta, view);
      debug_ui_.Build(renderer_, camera_, frame_delta, &view);
      renderer_.RenderFrame(view);
      // Test/CI hook: RX_UI_SHOT=<path> grabs the frame after
      // RX_UI_SHOT_FRAMES (default 30) and quits. Lets a headless GPU run
      // capture a frame without driving the app.
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
          renderer_.CaptureScreenshot(shot);
          // Perf breadcrumb for headless A/B runs alongside the capture.
          RX_INFO("gpu frame at capture: {:.2f} ms", renderer_.gpu_frame_ms());
        } else if (ui_shot_frames > ui_shot_target) {
          quit_.store(true, std::memory_order_relaxed);
        }
      }
    } else {
      // No vsync to pace the loop; yield between fixed steps instead of
      // spinning a core.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  return !quit_.load(std::memory_order_relaxed);
}

}  // namespace rx
