#ifndef RX_RUNTIME_DEMO_GYM_H_
#define RX_RUNTIME_DEMO_GYM_H_

#include <string>
#include <vector>

#include "core/input.h"
#include "core/input_actions.h"
#include "core/math.h"
#include "engine_context.h"
#include "character/character.h"
#include "inventory/item_catalog.h"
#include "inventory/world_item.h"
#include "render/core/renderer.h"

namespace rx {

// Character / inventory reference "gym" (--demo gym): a graybox playground with a
// classic dev-checker reference texture where a person walks around in first /
// third person and tunes eye heights, capsule dims and player scale against
// known-size geometry (0.25/0.5/1/2 m cubes, a 2.1 m doorway, 0.15/0.30 m stairs,
// 30/45/60 deg ramps, a crouch tunnel, furniture-scale obstacles, a narrow gap
// and a moving platform). It exercises engine/character (locomotion + FP/TP view
// modes over Jolt CharacterVirtual, with real camera-obstruction collision) and
// garnishes engine/inventory (drop/pick-up world items). An imgui panel exposes
// every tuning knob live.
//
// The gym owns the player camera: unlike the other demos it drives the scene
// camera rig instead of the free-fly camera, so the Viewer routes input to
// Update() and reads the resolved camera back through Emit().
class GymDemo {
 public:
  explicit GymDemo(EngineContext& ctx);

  // Builds the graybox world, the player entity (capsule + movement + view rig +
  // camera stack) and the inventory. Call once from DemoScenes::CreateDemoScene.
  void Create();

  // Per-frame input + fixed-ish simulation: fills the player's CharacterIntent
  // from input, runs the character + camera-rig pipeline in the README's staged
  // order, and maintains the dropped-item lifecycle. Called from the Viewer's
  // OnUpdate (windowed). `allow_*` are false while imgui owns that device.
  void Update(f32 dt, const InputState& input, const ActionState& actions, bool allow_keyboard,
              bool allow_mouse);

  // Writes the resolved character camera into the frame view, emits the player
  // proxy capsule (hidden in first person) and the moving platform, and draws
  // the tuning imgui panel. Called from DemoScenes::EmitToView.
  void Emit(f32 dt, render::FrameView& view);

  // Whether the OS cursor should be captured (relative-mouse look). The Viewer
  // mirrors this into the window each frame; Tab toggles it so the panel is
  // clickable.
  bool wants_mouse_capture() const { return mouse_captured_; }

 private:
  void BuildContent();
  void BuildPlayer();
  void FillIntent(const InputState& input, const ActionState& actions, bool allow_keyboard,
                  bool allow_mouse, f32 dt);
  void RunScript(f32 dt);
  void SyncViewSettingsToRig();
  void DrawPanel();
  void DropCrate();
  void PickUpNearest();
  void ResetPlayer();

  EngineContext& ctx_;

  ecs::Entity player_{};
  ecs::Entity camera_output_{};

  character::CharacterViewSettings view_settings_{};

  // Inventory garnish.
  inventory::ItemCatalog catalog_;
  inventory::WorldItemStore store_;
  inventory::ItemDefId crate_def_ = inventory::kInvalidItemDef;
  u64 crate_mesh_ = 0;

  // Player proxy capsule (cylinder body + two sphere caps, tinted).
  u64 capsule_body_mesh_ = 0;
  u64 capsule_cap_mesh_ = 0;

  // Moving platform (demonstration; the character module's platform-riding is
  // not folded in yet).
  ecs::Entity platform_{};
  physics::BodyId platform_body_ = 0;
  u64 platform_mesh_ = 0;
  Vec3 platform_center_{0, 0, 0};
  f32 platform_span_ = 3.0f;
  f32 platform_time_ = 0;

  Vec3 spawn_feet_{0, 0, 8};
  f32 spawn_yaw_ = 0;

  bool mouse_captured_ = true;
  f32 look_sensitivity_ = 0.0022f;  // radians / mouse pixel
  bool invert_pitch_ = false;

  // Resolved camera, cached in Update and applied in Emit.
  Vec3 cam_eye_{0, 1.6f, 10};
  Vec3 cam_target_{0, 1.6f, 0};
  f32 cam_fov_ = 1.0472f;
  bool cam_valid_ = false;

  // Env-gated scripted input for staged, headless-style captures (RX_GYM_SCRIPT).
  struct ScriptStep {
    f32 time = 0;
    std::string token;
  };
  std::vector<ScriptStep> script_;
  f32 script_time_ = 0;
  u32 script_cursor_ = 0;
  // Movement the active script segment holds until the next segment.
  f32 script_move_fwd_ = 0;
  f32 script_move_right_ = 0;
  bool script_sprint_ = false;
  bool script_crouch_ = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_GYM_H_
