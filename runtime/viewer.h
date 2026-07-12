#ifndef RX_RUNTIME_VIEWER_H_
#define RX_RUNTIME_VIEWER_H_

#include <cstdio>
#include <memory>
#include <string>

#include <base/containers/vector.h>

#include "anim/expression.h"
#include "app/application.h"
#include "app/host.h"
#include "asset/mesh.h"

#include "debug_ui.h"
#include "engine_context.h"
#include "fly_camera.h"
#include "showcase_camera.h"

namespace rx {

class DemoScenes;

// The rx viewer: the reference app::Application. Loads a glTF scene or a
// builtin demo, drives the free-fly camera and the scripted camera paths
// (orbit / record / replay / cinematic showcase), the day/night sun, the
// imgui debug overlay and the capture/benchmark hooks. Everything here is
// viewer policy; the subsystems and the loop live in app::Host.
class Viewer : public app::Application {
 public:
  explicit Viewer(const EngineConfig& config);
  ~Viewer() override;

  bool OnInitialize(app::Services& services) override;
  void OnUpdate(f32 frame_delta) override;
  void OnBuildView(f32 frame_delta, render::FrameView& view) override;
  void OnFrameEnd() override;
  void OnShutdown() override;

 private:
  bool LoadGltfScene();
  // Registers the small wooden cube every scene can throw around (F key).
  void CreatePhysicsCubeAsset();
  void ThrowPhysicsCube();
  // Derives the sun direction/intensity/color/ambient from the clock's time
  // of day, unless RX_SUN_DIR pinned a fixed sun or the scene staged its own.
  void DriveSunFromClock();
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

  // Engine services, cached from app::Services at OnInitialize. Owned by the
  // host; stable for its lifetime.
  app::Host* host_ = nullptr;
  Window* window_ = nullptr;
  render::Renderer* renderer_ = nullptr;
  ecs::World* world_ = nullptr;
  physics::PhysicsWorld* physics_ = nullptr;
  WorldClock* clock_ = nullptr;
  InputMap* input_map_ = nullptr;
  const ActionState* actions_ = nullptr;
  base::Vector<PhysicsEntity>* physics_entities_ = nullptr;

  FlyCamera camera_;
  DebugUi debug_ui_;

  // drive_sun_from_clock_ is false when RX_SUN_DIR pins a fixed sun (headless
  // lighting tests). last_sky_hour_ throttles the sun update so the IBL
  // environment is not rebuilt every frame for sub-degree motion.
  bool drive_sun_from_clock_ = true;
  f32 last_sky_hour_ = -1000.0f;

  asset::AssetId physics_cube_mesh_;

  // Morphed glTF instances, drawn by the viewer instead of the host's entity
  // gather so per-frame weights ride the DrawItems: a looping imported weight
  // track when the mesh has one, the expression controller when the targets
  // carry ARKit-style names, else a scripted sweep over the targets.
  struct MorphedInstance {
    u64 mesh = 0;
    Mat4 transform = Mat4::Identity();
    asset::MorphAnimation animation;  // empty times = no imported track
    base::Vector<f32> weights;        // dense per-target set, rewritten each frame
    base::Vector<i32> expression_map;  // controller channel -> target, empty = not driven
    bool pinned = false;               // RX_MORPH_WEIGHTS: weights fixed at load
  };
  void EmitMorphedInstances(f32 frame_delta, render::FrameView& view);
  base::Vector<MorphedInstance> morphed_;
  f32 morph_time_ = 0;

  // Expression demo: faces the controller recognizes cycle through the stock
  // poses with the life layer (blinks, brow micro-motion) always on.
  anim::ExpressionController expression_;
  bool expression_demo_ = false;
  u32 expression_pose_ = 0;
  f32 expression_hold_ = 0;

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

  // Shared service bundle handed to the demo scenes and the debug overlay.
  EngineContext ctx_;
  std::unique_ptr<DemoScenes> demos_;
};

}  // namespace rx

#endif  // RX_RUNTIME_VIEWER_H_
