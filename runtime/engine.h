#ifndef RX_RUNTIME_ENGINE_H_
#define RX_RUNTIME_ENGINE_H_

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/frame_timer.h"
#include "core/input_bindings.h"
#include "core/job_system.h"
#include "core/window.h"
#include "core/world_clock.h"

#include "demo_scenes.h"
#include "engine_context.h"
#include "showcase_camera.h"

namespace rx {

// Top-level orchestrator. Owns the shared services, the main loop and the
// camera; the demo scenes own their own state and are driven from here through
// the EngineContext.
class Engine {
 public:
  Engine() = default;
  ~Engine();

  // `window` lets a platform supply its own surface; when null the engine
  // creates one itself. A failed Initialize tears down whatever it had brought
  // up (the destructor calls Shutdown), so callers need not Shutdown after a
  // failure.
  bool Initialize(const EngineConfig& config, std::unique_ptr<Window> window = nullptr);
  int Run();
  // One iteration of the main loop. Returns false when the engine wants to
  // stop. Platforms that own the loop drive this directly instead of the
  // blocking Run().
  bool RunFrame();
  void Shutdown();

  // Surface lifecycle for platforms whose window can go away (mobile).
  void OnSurfaceDestroyed();
  void OnSurfaceCreated();

  // Safe to call from a signal handler; Run() returns after the current frame.
  void RequestQuit() { quit_.store(true, std::memory_order_relaxed); }

 private:
  bool LoadGltfScene();
  // Resolves the configured quality tier from the gpu (or a forced preset) and
  // applies it to the renderer's live settings.
  void ApplyRenderPreset();
  // (Re)seeds the day/night clock. RX_TIMESCALE / RX_GAME_HOUR override the
  // timescale and start hour.
  void ConfigureClock(f32 base_timescale);

  void ThrowPhysicsCube();
  void UpdateCamera(f32 frame_delta);
  // Camera record/replay (deterministic playback for benchmarks and capture).
  // RX_ORBIT turntables the camera, RX_RECORD=<path> writes the path each
  // frame, RX_REPLAY=<path> drives the camera from a recorded path.
  void DriveCamera(f32 dt);
  void LookCameraAt(const Vec3& eye, const Vec3& center);
  // Builds the cinematic showcase path (RX_SHOWCASE): a smooth drone pass over
  // the loaded scene.
  void BuildShowcase();

  EngineConfig config_;

  std::unique_ptr<Window> window_;
  std::unique_ptr<JobSystem> jobs_;
  FrameTimer timer_;
  // The clock driving the day/night cycle; the render loop derives the sun/sky
  // from it. drive_sun_from_clock_ is false when RX_SUN_DIR pins a fixed sun
  // (headless lighting tests). last_sky_hour_ throttles the sun update so the
  // IBL environment is not rebuilt every frame for sub-degree motion.
  WorldClock clock_;
  bool drive_sun_from_clock_ = true;
  f32 last_sky_hour_ = -1000.0f;

  ecs::World world_;
  ecs::Scheduler scheduler_;

  asset::Vfs vfs_;
  // Audio: SDL-backed mixer + decoders, fed sound bytes through the Vfs.
  std::unique_ptr<audio::AudioSystem> audio_;
  std::unique_ptr<asset::AssetDatabase> assets_;

  render::Renderer renderer_;
  FlyCamera camera_;
  // Device-agnostic input: bindings + the per-frame resolved action snapshot.
  InputMap input_map_;
  ActionState actions_;

  // Camera record/replay state, lazily armed from env on the first frame.
  struct CamKey {
    f32 t = 0;
    Vec3 pos{};
    Vec3 target{};
  };
  bool cam_init_ = false;
  bool cam_orbit_ = false;
  f32 cam_time_ = 0;
  std::FILE* cam_record_ = nullptr;
  base::Vector<CamKey> cam_replay_;

  // Cinematic showcase (RX_SHOWCASE): a smooth drone flythrough over the
  // scene, doubling as a deterministic benchmark and a source of regression
  // frames (RX_SHOWCASE_SHOTS=<dir>).
  ShowcaseCamera showcase_;
  bool cam_showcase_ = false;
  bool showcase_done_ = false;
  bool showcase_quit_ = false;  // RX_SHOWCASE_QUIT: exit when the pass ends
  std::string showcase_shot_dir_;
  f32 showcase_dt_min_ = 1e9f;
  f32 showcase_dt_max_ = 0;
  f32 showcase_bench_time_ = 0;  // summed dt of benchmarked frames (excludes load hitches)
  u32 showcase_frames_ = 0;

  DebugUi debug_ui_;
  int screenshot_index_ = 0;

  physics::PhysicsWorld physics_;
  // Dynamic bodies mirrored into ECS transforms after each step.
  base::Vector<PhysicsEntity> physics_entities_;
  asset::AssetId physics_cube_mesh_;

  // Last frame's world matrices keyed by entity, for motion vectors.
  base::UnorderedMap<u64, Mat4> prev_transforms_;

  // Shared service bundle handed to the subsystems, plus the subsystems
  // themselves (built in Initialize once the context is populated).
  EngineContext ctx_;
  std::unique_ptr<DemoScenes> demos_;

  std::atomic<bool> quit_ = false;
  bool shut_down_ = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_ENGINE_H_
