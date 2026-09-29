#ifndef RX_APPS_SHELL_DEMOS_PUPPET_H_
#define RX_APPS_SHELL_DEMOS_PUPPET_H_

#include <base/containers/vector.h>

#include "foundation/math/math.h"
#include "plugins/locomotion/controller.h"
#include "rxe/ecs/entity.h"
#include "rxe/ecs/scheduler.h"
#include "rxe/ecs/world.h"
#include "rxe/physics/physics_world.h"
#include "rxe/render/core/renderer.h"
#include "rxe/scene/fly_camera.h"
#include "rxe/ui/events/input.h"
#include "viewer_config.h"

namespace rx::shell {

// Physics-first locomotion proving ground (--demo puppet): a graybox arena
// (flat floor, a shallow ramp, a 0.15 m curb, a few scattered boxes) with a
// single rx::locomotion ragdoll biped standing on it. The controller Ticks once
// per fixed step in the kPreSim stage (BEFORE the host steps the shared physics
// world in kSim, so the demo never double-steps Jolt); a scripted intent walks
// it hands-free (stand -> walk -> turn -> stop -> loop). Each of the 13 rig
// bodies renders as a box proxy posed from physics.GetBodyTransform, and the
// controller's DebugState is drawn every frame as a render::DebugLine overlay
// (capture point, support region, foot targets/swing, COM velocities).
class PuppetDemo {
 public:
  PuppetDemo(const ViewerConfig* config, ecs::World* world, ecs::Scheduler* scheduler, render::Renderer* renderer, scene::FlyCamera* camera, physics::PhysicsWorld* physics);

  // Builds the arena + the controller and registers the fixed-step tick system.
  // Call once from DemoScenes::CreateDemoScene.
  void Create();

  // Poses the 13 body proxies and emits the debug overlay for this frame.
  // Called from DemoScenes::EmitToView.
  void Emit(f32 dt, render::FrameView& view);

  // Optional raw-key interaction, routed from Viewer::OnUpdate (mirroring the
  // gym's input route, but the puppet keeps the free-fly camera). 1 = small
  // push, 2 = big push (controlled fall), 3 = reset. The actual impulse/reset is
  // deferred to the next fixed tick so it lands before the physics step.
  void OnInput(const ui::InputState& input, bool allow_keyboard);

 private:
  void BuildArena();
  void BuildProxies();
  void ResetPuppet();
  void Step(f32 dt);  // kPreSim: scripted intent -> pending pushes -> Tick
  locomotion::LocomotionIntent ScriptedIntent() const;
  void EmitDebugLines(render::FrameView& view);

  const ViewerConfig* config_;
  ecs::World* world_;
  ecs::Scheduler* scheduler_;
  render::Renderer* renderer_;
  scene::FlyCamera* camera_;
  physics::PhysicsWorld* physics_;
  locomotion::LocomotionController controller_;
  locomotion::ControllerParameters params_{};
  Vec3 spawn_feet_{0, 0.02f, 0};
  f32 spawn_yaw_ = 0;

  ecs::Entity proxy_[locomotion::kBodyPartCount] = {};

  f32 script_time_ = 0;
  u32 rng_ = 0x1234567u;
  locomotion::ControlMode last_mode_ = locomotion::ControlMode::kStable;
  bool mode_logged_ = false;

  // Raw-key requests, consumed on the next fixed tick (before physics steps).
  bool pending_small_push_ = false;
  bool pending_big_push_ = false;
  bool pending_reset_ = false;
  bool prev_key_[3] = {};  // edge detection for 1/2/3

  base::Vector<render::DebugLine> lines_;
  bool draw_lines_ = true;
};

}  // namespace rx::shell

#endif  // RX_APPS_SHELL_DEMOS_PUPPET_H_
