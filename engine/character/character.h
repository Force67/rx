#ifndef RX_CHARACTER_CHARACTER_H_
#define RX_CHARACTER_CHARACTER_H_

#include "core/export.h"
#include "core/math.h"
#include "ecs/entity.h"
#include "physics/physics_world.h"
#include "scene/camera.h"

namespace rx::ecs {
class World;
}
namespace rx::physics {
class PhysicsWorld;
}

// Engine-level, ECS-driven player-character support: a capsule locomotion
// controller (walk/run/sprint/crouch/jump over Jolt CharacterVirtual) plus
// first/third-person view modes composed from the scene camera rig. Components
// are plain data; every system is a free function staged like the camera rig.
// No game-specific naming or behaviour lives here.
//
// Coordinate convention (shared with scene::): right-handed, +Y up. Look yaw 0
// faces -Z and yaw increases the same way scene::CameraOrbit's does, so a
// character heading and its camera agree. Horizontal move input is world-space:
// the game rotates stick input into world before filling CharacterIntent, which
// keeps the controller free of any camera/heading policy.
namespace rx::character {

// ---------------------------------------------------------------------------
// Components (data only).
// ---------------------------------------------------------------------------

// The locomotion tuning. Per-gait ground speeds, how quickly velocity chases
// the target, how much authority exists in the air, jump height (the vertical
// impulse is derived from `gravity`), and the stair/slope limits forwarded to
// the physics controller.
struct CharacterMovementSettings {
  f32 walk_speed = 1.6f;    // m/s
  f32 run_speed = 4.2f;     // m/s
  f32 sprint_speed = 6.5f;  // m/s
  f32 crouch_speed = 1.5f;  // m/s (applies in any gait while crouched)

  f32 ground_acceleration = 45.0f;  // m/s^2 toward the gait target on the ground
  f32 ground_deceleration = 55.0f;  // m/s^2 back toward rest when input releases
  f32 air_control = 0.35f;          // [0..1] scales ground_acceleration while airborne

  f32 jump_height = 1.1f;  // apex height in metres; impulse = sqrt(2*gravity*height)
  f32 gravity = 16.0f;     // m/s^2, integrated by the controller (~1.6g: brisk, non-floaty arc)

  f32 step_height = 0.4f;              // tallest ledge the controller steps over
  f32 max_slope_angle = 0.9599311f;    // ~55 deg, steepest walkable ground

  // --- Game-feel: turn smoothing (never robotic) ------------------------------
  // The body facing yaw (visible mesh / Transform rotation) eases toward the
  // movement direction instead of snapping. Only active in third person; first
  // person hard-locks facing to the raw look yaw (see StepCharacters). A near-180
  // input reversal past `pivot_angle` uses the faster `pivot_turn_half_life` so a
  // reversal spins on the spot instead of arcing mushily. Half-life = seconds to
  // close half the remaining angle; 0 snaps (old instant behaviour).
  f32 turn_half_life = 0.09f;         // s, eased facing chase (fast, responsive)
  f32 pivot_turn_half_life = 0.05f;   // s, faster chase for reversals past pivot_angle
  f32 pivot_angle = 2.4434610f;       // rad (~140 deg): beyond this the pivot rate applies

  // --- Game-feel: gait target-speed blend -------------------------------------
  // The *target* speed eases across gait changes (walk<->run<->sprint) over
  // `speed_blend_time` (time to traverse the full walk..sprint span; partial
  // changes are proportionally quicker) so speed does not step. Ground
  // acceleration itself stays snappy. 0 steps instantly (old behaviour).
  f32 speed_blend_time = 0.18f;       // s
  f32 stop_speed_epsilon = 0.05f;     // m/s: below this with no input, velocity zeroes exactly

  // --- Game-feel: jump forgiveness --------------------------------------------
  // Jump buffer: a jump pressed up to `jump_buffer_time` before touchdown still
  // fires on landing. Coyote time: a jump within `coyote_time` after walking off
  // a ledge still fires. Both 0 disable the window (jump only when grounded).
  f32 jump_buffer_time = 0.12f;       // s
  f32 coyote_time = 0.12f;            // s
};

// The capsule + eye geometry for each stance. Heights are TOTAL capsule heights
// (tip to tip); the controller derives Jolt's cylinder half-height as
// height/2 - radius. Eye heights are measured from the feet.
struct CharacterShape {
  f32 standing_radius = 0.3f;
  f32 standing_height = 1.8f;
  f32 crouched_radius = 0.3f;
  f32 crouched_height = 1.2f;

  f32 standing_eye_height = 1.62f;
  f32 crouched_eye_height = 0.95f;

  f32 crouch_blend_speed = 9.0f;  // crouch fraction change per second

  // --- Game-feel: eye/anchor vertical smoothing -------------------------------
  // The camera anchor's *vertical* component eases over sudden ground-height
  // changes (step-up / step-down / stairs) so the eye glides instead of popping;
  // horizontal stays 1:1 raw (smoothing horizontal reads as lag). Active only
  // while grounded — airborne (jumps) snap so the arc is not damped. Half-life in
  // seconds; 0 disables (raw eye, old behaviour).
  f32 eye_step_half_life = 0.06f;

  // --- Game-feel: landing recoil ----------------------------------------------
  // On touchdown after a real fall, a small fast-recovering eye-height dip scaled
  // by impact speed. No screen shake. `landing_dip_scale` 0 disables it.
  f32 landing_dip_min_speed = 2.5f;   // m/s impact below which no dip
  f32 landing_dip_scale = 0.03f;      // metres of dip per (m/s) of impact over the min
  f32 landing_dip_max = 0.14f;        // metres, hard cap (kept subtle)
  f32 landing_dip_half_life = 0.09f;  // s, recovery half-life
};

enum class CharacterGait : u8 { kWalk, kRun, kSprint };

// Written by the game every fixed step, consumed (angular/edge fields cleared)
// by StepCharacters.
struct CharacterIntent {
  // World-space horizontal move request. Direction is the desired heading;
  // magnitude in [0..1] is analog throttle (>1 is clamped). Y is ignored.
  Vec3 move = {0, 0, 0};
  CharacterGait gait = CharacterGait::kRun;
  bool crouch = false;  // desired crouch stance (game owns hold-vs-toggle)
  bool jump = false;    // rising edge this step

  // Look deltas (radians). Yaw updates the character heading; pitch is forwarded
  // to a co-located scene::CameraIntent for the camera rig to accumulate/clamp.
  f32 look_yaw_delta = 0;
  f32 look_pitch_delta = 0;
};

enum class CharacterStance : u8 {
  kStanding,
  kCrouching,
  // Reserved for future titles; not driven by StepCharacters yet.
  kSwimming,
  kFlying,
  kProne,
};

// Controller output, read by animation, the camera bridge and gameplay.
struct CharacterState {
  CharacterStance stance = CharacterStance::kStanding;
  bool grounded = false;
  Vec3 velocity = {0, 0, 0};  // full world-space velocity (horizontal + vertical)
  f32 crouch_blend = 0;       // [0..1], 0 standing, 1 fully crouched
  f32 eye_height = 1.62f;     // current, blended, measured from the feet
  f32 time_since_grounded = 0;  // seconds airborne (coyote-time friendly)
  f32 yaw = 0;                  // LOOK yaw, radians; raw (never smoothed) — feeds the camera anchor
  bool teleported = false;      // set by TeleportCharacter; bumps the anchor revision once

  // --- Game-feel state (written by StepCharacters) ----------------------------
  f32 facing_yaw = 0;      // BODY facing, radians; drives Transform. Eases toward the
                           // movement dir in third person, hard-locked to `yaw` in first.
  bool pivoting = false;   // in a quick-pivot (near-180 reversal): holds the faster turn rate
  f32 gait_speed = 0;      // smoothed target gait speed (m/s), blended across gait changes
  f32 jump_buffer_timer = 0;   // seconds of remaining buffered-jump window
  bool jump_consumed = false;  // a jump fired this airborne stint (blocks coyote double-jumps)
  f32 eye_base_y = 0;      // step-smoothed world-space eye Y (before landing dip)
  f32 anchor_eye_y = 0;    // eye_base_y minus landing dip — the value the camera anchor reads
  f32 landing_dip = 0;     // current landing-recoil dip in metres (>= 0), decays to 0
  bool view_initialized = false;  // facing_yaw / eye Y primed (cleared on teleport to snap)
};

// Links the entity to its Jolt CharacterVirtual and tracks the live capsule
// dimensions (updated as the crouch blend resizes it).
struct CharacterBody {
  physics::CharacterId id = 0;
  f32 radius = 0.3f;
  f32 half_height = 0.6f;  // Jolt cylinder half-height (excludes the hemispheres)
  bool configured = false;  // ConfigureCharacter pushed once on first step
};

enum class CharacterViewKind : u8 { kFirstPerson, kThirdPerson };

// Selects which view recipe the entity presents. The toggle/zoom helpers below
// switch this and re-compose the rig components.
struct CharacterViewMode {
  CharacterViewKind kind = CharacterViewKind::kFirstPerson;
  // Bookkeeping for ToggleCharacterViewMode: the stack activation of the mode
  // it pushed to drive the last FP<->TP transition, retired on the next toggle
  // so the stack stays bounded. Not written by games directly.
  scene::CameraActivation transition;
};

// ---------------------------------------------------------------------------
// Systems (free functions, staged per fixed step).
// ---------------------------------------------------------------------------

// Stage A. Consume CharacterIntent: update heading yaw, accelerate the velocity
// toward the gait target (ground accel vs air control), integrate gravity and
// jump, apply stance changes (crouch enter is free; crouch exit needs headroom,
// probed with an upward sphere cast), resize the capsule feet-planted across the
// crouch blend, drive the physics controller and write back scene::Transform +
// CharacterState. Runs on every entity with the full character component set.
RX_CHARACTER_EXPORT void StepCharacters(ecs::World& world, physics::PhysicsWorld& physics, f32 dt);

// Stage B. For entities with both a CharacterState and a scene::CameraAnchor,
// write the anchor (feet + eye-height position, heading orientation, velocity,
// revision bump on teleport). Run after StepCharacters, before BuildCameraRigs.
RX_CHARACTER_EXPORT void SyncCharacterCameraAnchors(ecs::World& world);

// The physics bridge that makes third-person camera collision real: for each
// scene::CameraObstruction carrying a fresh request id, sphere-cast from origin
// toward desired_position and answer with SetCameraObstructionResult. Run after
// PrepareCameraRigConstraints, before ResolveCameraRigs.
RX_CHARACTER_EXPORT void AnswerCameraObstructions(ecs::World& world, physics::PhysicsWorld& physics);

// ---------------------------------------------------------------------------
// View-mode composition helpers (compose EXISTING scene rig components).
// ---------------------------------------------------------------------------

// Tuning for the two recipes. Defaults are engine-reasonable; a game tunes them.
struct CharacterViewSettings {
  // First person.
  f32 fp_pitch_limit = 1.4835f;  // ~85 deg

  // Third person boom.
  f32 tp_distance = 3.25f;
  f32 tp_min_distance = 1.4f;
  f32 tp_max_distance = 6.0f;
  f32 tp_shoulder_offset = 0.45f;
  f32 tp_height_offset = 0.15f;
  f32 tp_pitch_min = -1.1f;
  f32 tp_pitch_max = 1.2f;
  f32 tp_position_half_life = 0.08f;
  f32 tp_rotation_half_life = 0.05f;
  f32 tp_obstruction_radius = 0.25f;
  f32 tp_obstruction_margin = 0.12f;
};

// Installs / rewrites the rig components on `entity` for its current
// CharacterViewMode.kind. First person: eye-anchored orbit (yaw from the anchor,
// pitch clamped), no boom, no lag. Third person: orbit + boom + obstruction +
// damping. Idempotent; safe to call after a mode switch. Structural (adds/removes
// components) so call it outside World::Each.
RX_CHARACTER_EXPORT void ApplyCharacterViewMode(ecs::World& world, ecs::Entity entity,
                                                const CharacterViewSettings& settings = {});

// Switches the entity to `kind` (if different), re-applies the recipe and drives
// a camera-stack transition on `output` through the given `mode` entity so the
// FP<->TP cut is smooth. Returns the push result; kInvalidActivation-style
// results mean the stack was not set up. No-op transition when already in `kind`.
RX_CHARACTER_EXPORT scene::CameraStackResult ToggleCharacterViewMode(
    ecs::World& world, ecs::Entity entity, ecs::Entity output, ecs::Entity mode,
    const CharacterViewSettings& settings = {},
    scene::CameraTransitionSpec transition = {});

// Optional: fold scroll `zoom_delta` into the third-person boom distance within
// [min,max]. When `allow_mode_switch` is set, zooming past the minimum switches
// the entity to first person and zooming out of first person restores third
// person, re-applying the recipe. Returns true when the view kind changed.
RX_CHARACTER_EXPORT bool ApplyCharacterZoom(ecs::World& world, ecs::Entity entity, f32 zoom_delta,
                                            bool allow_mode_switch = false,
                                            const CharacterViewSettings& settings = {});

// Marks the controller teleported: snaps the physics capsule to `feet_position`,
// resets velocity and requests a one-shot anchor revision bump so the camera
// cuts instead of interpolating across the jump.
RX_CHARACTER_EXPORT void TeleportCharacter(ecs::World& world, physics::PhysicsWorld& physics,
                                           ecs::Entity entity, const Vec3& feet_position);

}  // namespace rx::character

#endif  // RX_CHARACTER_CHARACTER_H_
