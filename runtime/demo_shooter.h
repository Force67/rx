#ifndef RX_RUNTIME_DEMO_SHOOTER_H_
#define RX_RUNTIME_DEMO_SHOOTER_H_

#include <string>

#include <base/containers/vector.h>

#include "combat/damage.h"
#include "combat/events.h"
#include "combat/weapon.h"
#include "core/input.h"
#include "core/input_actions.h"
#include "core/math.h"
#include "engine_context.h"
#include "render/core/renderer.h"

namespace rx {

// The FPS range (--demo shooter): a first-person shooting gallery that drives
// engine/combat end to end. A player walks the range on the character
// controller, carries four weapons cut from the same WeaponDef data (an
// automatic rifle that punches through thin cover, a pellet shotgun that
// reloads shell by shell, a semi-auto marksman rifle with real aim-down-sights,
// and a projectile launcher whose grenades arc and detonate), and shoots
// popup targets with head and torso hitboxes, moving targets that have to be
// led, and physics crates that take the impulse.
//
// Everything a shooter needs to feel right is on show: rate of fire, spread
// bloom that opens when you move and closes when you aim, view recoil that
// climbs and settles, magazine and reserve ammo, weapon swap timing, damage
// falloff, head-shot multipliers, blast falloff behind cover, and a viewmodel
// that sways and bobs. The HUD is imgui; the tuning panel edits every WeaponDef
// field live so the feel can be dialled in without a rebuild.
//
// Like the gym, this demo owns the player camera and input: the Viewer routes
// OnUpdate here and reads the resolved camera back through Emit().
class ShooterDemo {
 public:
  explicit ShooterDemo(EngineContext& ctx);

  // Builds the range, the weapon catalog, the targets and the player. Call once
  // from DemoScenes::CreateDemoScene.
  void Create();

  // Per-frame input + fixed-step simulation: look/move/fire intent, the
  // character + camera pipeline, the combat systems, target lifecycles and the
  // presentation queues drained from CombatEvents.
  void Update(f32 dt, const InputState& input, const ActionState& actions, bool allow_keyboard,
              bool allow_mouse);

  // Writes the resolved camera into the frame view, emits the range geometry,
  // targets, crates, viewmodel, tracers and impact marks, and draws the HUD.
  void Emit(f32 dt, render::FrameView& view);

  bool wants_mouse_capture() const { return mouse_captured_; }

 private:
  // One piece of static range geometry: a scaled unit cube with a tint.
  struct Prop {
    Mat4 transform;
    u32 tint = 0;
  };

  // A popup target: an entity carrying the Health, plus the two kinematic
  // hitbox bodies registered with the combat HitRegistry.
  struct Target {
    ecs::Entity entity{};
    physics::BodyId torso = 0;
    physics::BodyId head = 0;
    Vec3 home{};
    f32 respawn_timer = 0;
    f32 strafe_span = 0;  // 0 = a static target
    f32 strafe_phase = 0;
  };

  // A dynamic crate: shootable scenery that takes bullet and blast impulses.
  struct Crate {
    physics::BodyId body = 0;
    Vec3 half_extent{};
    u32 tint = 0;
  };

  // Presentation, all driven off CombatEvents.
  struct Tracer {
    Vec3 from;
    Vec3 to;
    f32 life = 0;
  };
  struct Mark {
    Mat4 transform;
    f32 life = 0;
  };
  struct Popup {
    Vec3 position;
    std::string text;
    u32 rgba = 0xffffffff;
    f32 life = 0;
  };

  void BuildRange();
  void BuildWeapons();
  void BuildPlayer();
  void SpawnTarget(const Vec3& feet, f32 strafe_span);
  void FillLookAndMove(const InputState& input, const ActionState& actions, bool allow_keyboard,
                       bool allow_mouse, f32 dt);
  void FillWeaponIntent();
  void StepTargets(f32 dt);
  void DrainEvents();
  void AgePresentation(f32 dt);
  void EmitViewmodel(render::FrameView& view);
  void DrawHud();
  void ResetPlayer();

  EngineContext& ctx_;

  ecs::Entity player_{};
  ecs::Entity camera_output_{};

  combat::WeaponCatalog catalog_;
  combat::HitRegistry registry_;
  combat::CombatEvents events_;
  combat::WeaponDefId rifle_ = 0;
  combat::WeaponDefId shotgun_ = 0;
  combat::WeaponDefId marksman_ = 0;
  combat::WeaponDefId launcher_ = 0;

  base::Vector<Prop> props_;
  base::Vector<Target> targets_;
  base::Vector<Crate> crates_;

  base::Vector<Tracer> tracers_;
  base::Vector<Mark> marks_;
  base::Vector<Popup> popups_;
  base::Vector<render::DebugLine> lines_;
  f32 hitmarker_ = 0;
  u32 kills_ = 0;
  u32 hits_ = 0;
  u32 rounds_ = 0;

  u64 cube_mesh_ = 0;
  u64 sphere_mesh_ = 0;

  // Edge-triggered input latched on the render frame, consumed by the first
  // fixed step that follows.
  bool pending_reload_ = false;
  i8 pending_switch_ = -1;

  // RX_SHOOTER_AUTOFIRE=1: hold the trigger and point at the nearest live
  // target every step. A capture hook (like the gym's RX_GYM_SCRIPT), so a
  // headless or screenshot run exercises firing, impacts and kills without a
  // hand on the mouse.
  bool autofire_ = false;

  bool mouse_captured_ = true;
  f32 look_sensitivity_ = 0.0022f;
  bool invert_pitch_ = false;
  bool show_panel_ = true;

  Vec3 spawn_feet_{0, 0.05f, 8.0f};

  Vec3 cam_eye_{0, 1.6f, 8.0f};
  Vec3 cam_target_{0, 1.6f, 0};
  Quat cam_orientation_{0, 0, 0, 1};
  f32 cam_fov_ = 1.0472f;
  bool cam_valid_ = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_SHOOTER_H_
