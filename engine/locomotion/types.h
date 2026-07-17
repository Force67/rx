#ifndef RX_LOCOMOTION_TYPES_H_
#define RX_LOCOMOTION_TYPES_H_

// Typed data for the physics-first locomotion controller (docs/LOCOMOTION.md).
// Everything here is a plain numeric struct: the controller is a feedback loop
// over the simulated body, so its inputs express goals and its intermediates
// express measured or planned physical state. There is deliberately no
// animation-resource identity anywhere in these types.
//
// Conventions (shared with rx::scene / rx::character): right-handed, +Y up,
// facing yaw 0 looks down -Z. Metres, m/s, kg, radians, N·m. All state is
// sampled/advanced only in the fixed physics step.

#include "core/math.h"
#include "core/types.h"

namespace rx::locomotion {

// ---------------------------------------------------------------------------
// Rig topology. Fixed simplified biped: 13 bodies, 12 joints.
// ---------------------------------------------------------------------------

enum class BodyPart : u8 {
  kPelvis,  // the root body
  kTorso,
  kHead,
  kUpperLegL,
  kLowerLegL,
  kFootL,
  kUpperLegR,
  kLowerLegR,
  kFootR,
  kUpperArmL,
  kLowerArmL,
  kUpperArmR,
  kLowerArmR,
  kCount,
};
inline constexpr u32 kBodyPartCount = static_cast<u32>(BodyPart::kCount);

enum class RigJoint : u8 {
  kWaist,  // pelvis -> torso (swing-twist)
  kNeck,   // torso -> head (swing-twist)
  kHipL,   // pelvis -> upper leg (swing-twist)
  kKneeL,  // upper leg -> lower leg (hinge)
  kAnkleL, // lower leg -> foot (swing-twist, tight limits)
  kHipR,
  kKneeR,
  kAnkleR,
  kShoulderL,  // torso -> upper arm (swing-twist)
  kElbowL,     // upper arm -> lower arm (hinge)
  kShoulderR,
  kElbowR,
  kCount,
};
inline constexpr u32 kRigJointCount = static_cast<u32>(RigJoint::kCount);

// Feet index the paired arrays below: 0 = left, 1 = right.
inline constexpr u32 kFootCount = 2;

// ---------------------------------------------------------------------------
// Inputs: goals and physical modifiers, filled by the game every fixed step.
// ---------------------------------------------------------------------------

// What the player or AI wants the body to do. Goals only — no animation names,
// no movement categories, no clip variants.
struct LocomotionIntent {
  Vec3 desired_velocity{};      // planar (y ignored), world space, m/s
  Vec3 desired_facing{0, 0, -1};  // planar unit-ish direction the chest should face
  f32 desired_body_height = 0;  // sole-to-crown target; 0 = parameters nominal
  bool request_crouch = false;
  bool allow_recovery = true;   // permit automatic get-up from kGrounded
};

// Continuous physical modifiers (surface, encumbrance, condition). Typed
// inputs passed every tick — never entries in a shared generic dictionary.
struct PhysicalModifiers {
  f32 traction = 1;   // scales usable foot friction assumptions [0..1+]
  f32 strength = 1;   // scales motor torque budgets
  f32 balance = 1;    // scales recovery margins (0.5 = tipsy, 1 = nominal)
  f32 carried_mass = 0;                 // kg added to the COM model
  Vec3 carried_mass_local_offset{};     // in pelvis space
};

// One flat resolved configuration per controller instance, complete at spawn.
// There is no inheritance and no fallback lookup; build tools may copy shared
// presets, but the runtime receives one resolved object.
struct ControllerParameters {
  // Body geometry and mass. The rig builder derives every segment dimension
  // and segment mass from these via fixed anthropometric fractions (rig.cc).
  f32 total_mass = 75;      // kg
  f32 leg_length = 0.9f;    // hip pivot to sole
  f32 hip_width = 0.24f;    // lateral distance between the hip pivots
  f32 body_height = 1.75f;  // sole to crown at rest

  // Gait envelope.
  f32 walk_speed = 1.5f;         // m/s, nominal
  f32 run_speed = 4.5f;          // m/s, top of the gait curve
  f32 max_acceleration = 8;      // m/s^2 toward the desired planar velocity
  f32 max_turn_rate = 3;         // rad/s facing chase
  f32 walk_stride_frequency = 1.5f;  // full left+right cycles per second at walk_speed
  f32 run_stride_frequency = 2.4f;   // cycles per second at run_speed
  f32 stance_fraction_walk = 0.62f;  // fraction of a foot's cycle spent in stance
  f32 stance_fraction_run = 0.40f;

  // Step geometry.
  f32 step_height = 0.12f;      // swing apex clearance above the chord
  f32 foot_clearance = 0.02f;   // minimum sole clearance during swing
  f32 max_step_length = 0.6f;
  f32 max_step_width = 0.5f;
  f32 max_step_up = 0.35f;      // tallest accepted terrain rise for one step
  f32 max_step_down = 0.5f;
  f32 max_ground_slope = 0.7f;  // radians; steeper probe hits reject the step

  // Balance / task-space gains.
  f32 pelvis_position_gain = 70;   // 1/s^2 on pelvis-height error
  f32 pelvis_velocity_gain = 12;   // 1/s on pelvis vertical velocity
  f32 torso_orientation_gain = 320; // N·m/rad equivalent drive scale
  f32 torso_angular_damping = 18;
  f32 capture_gain = 1.0f;         // scales the capture-point step correction
  f32 step_velocity_gain = 0.12f;  // scales (desired - measured) velocity into the step
  f32 lookahead_time = 0.18f;      // s of desired velocity baked into a step target

  // Joint drive (motor springs). Frequency in Hz, damping ratio ~1 critical.
  f32 joint_frequency = 10;
  f32 joint_damping = 1;
  f32 max_joint_torque = 400;   // N·m budget for stance legs; groups scale down
  f32 arm_drive_scale = 0.25f;  // arms and spine run softer than stance legs
  f32 root_assist_torque = 320; // N·m upright-assist budget applied at the pelvis

  // Recovery thresholds.
  f32 recovery_margin = 0.22f;     // m the capture point may leave the support region
  f32 fall_pitch_limit = 1.1f;     // rad of sustained torso tilt that forces kControlledFall
  f32 grounded_height_fraction = 0.45f;  // COM below this * leg_length => possibly grounded
  f32 grounded_dwell_time = 0.4f;  // s of low velocity + torso contact => kGrounded
  f32 recovery_blend_time = 0.35f; // s to blend motor strength across mode changes
};

// ---------------------------------------------------------------------------
// Measured state. Filled at the start of every fixed update by estimator.cc —
// measured from the simulation, never assumed from the previous plan.
// ---------------------------------------------------------------------------

struct FootMeasurement {
  Vec3 position{};        // sole centre, world
  Vec3 velocity{};        // world, m/s
  Vec3 contact_normal{0, 1, 0};
  f32 contact_impulse = 0;  // kg·m/s accumulated over the last step
  f32 slip_speed = 0;       // tangential speed while in contact, m/s
  bool in_contact = false;
};

struct CharacterMeasurements {
  Vec3 root_position{};   // pelvis body origin, world
  Quat root_rotation{};
  Vec3 root_linear_velocity{};
  Vec3 root_angular_velocity{};

  Vec3 com_position{};    // mass-weighted over all rig bodies (+ carried mass)
  Vec3 com_velocity{};

  FootMeasurement foot[kFootCount];  // 0 = left, 1 = right

  Vec3 ground_normal{0, 1, 0};  // blended from supporting foot contacts
  f32 estimated_body_height = 0;  // sole-to-crown along the ground normal
  f32 gravity = 9.81f;            // measured |world gravity|, m/s^2
  bool valid = false;             // false when any body read failed or non-finite
};

// Per-foot contact classification with hysteresis (estimator.cc). A foot must
// not flicker between supporting and swinging on one noisy frame.
enum class FootPhase : u8 { kSupporting, kSliding, kSwinging, kUnconfirmed };

struct ContactEstimate {
  FootPhase phase[kFootCount] = {FootPhase::kUnconfirmed, FootPhase::kUnconfirmed};
  f32 phase_time[kFootCount] = {0, 0};  // seconds in the current phase
  Vec3 support_center{};  // mean of supporting-foot positions (world)
  u32 support_count = 0;  // number of supporting feet
};

// ---------------------------------------------------------------------------
// Plans and targets, regenerated every tick.
// ---------------------------------------------------------------------------

struct GaitState {
  f32 phase = 0;        // [0,1) continuous gait phase; left foot keys `phase`,
                        // right foot keys wrap01(phase + 0.5)
  f32 phase_rate = 0;   // cycles per second currently applied
  f32 stance_fraction = 0.6f;  // current speed-blended stance share
  f32 speed_ratio = 0;  // 0 at standstill .. 1 at run_speed
  bool stepping = false;  // false when parked in double support
};

enum class StepReject : u8 {
  kNone,
  kNoGroundHit,
  kTooSteep,
  kTooHigh,
  kTooFar,
};

// One foot's plan for the current tick.
struct FootPlan {
  Vec3 target{};          // world-space landing target, terrain-projected
  Vec3 swing_position{};  // where the sole should be right now along the swing
  Vec3 swing_velocity{};
  Quat foot_orientation{};  // world target for the foot body
  f32 swing_progress = 0;   // [0,1] within the current swing, 0 in stance
  bool swinging = false;
  StepReject rejected = StepReject::kNone;  // why the step shortened/aborted
};

// Continuous numeric targets for the whole body — the only thing the joint
// layer consumes. Joint targets are LOCAL child-relative-to-parent rotations
// expressed relative to the bind pose (identity = bind); rig.cc converts them
// to constraint-space motor targets.
struct WholeBodyTargets {
  Quat joint_target[kRigJointCount]{};
  f32 joint_drive_scale[kRigJointCount]{};  // [0..1+] scales frequency/torque budget
  Vec3 root_assist_torque{};  // world N·m upright assist applied at the pelvis
  Vec3 root_assist_force{};   // world N, small COM tracking assist
  FootPlan foot[kFootCount];
};

// ---------------------------------------------------------------------------
// Control regime. A physical-state machine only — owns no resources, selects
// no animation content.
// ---------------------------------------------------------------------------

enum class ControlMode : u8 {
  kStable,
  kCorrectiveStep,
  kControlledFall,
  kGrounded,
  kRecovering,
};

// Numeric snapshot for debug drawing / logging; every instability should be
// diagnosable from this without a debugger (docs/LOCOMOTION.md).
struct DebugState {
  Vec3 desired_velocity{};
  Vec3 controlled_facing{0, 0, -1};
  Vec3 measured_velocity{};
  Vec3 com_position{};
  Vec3 com_velocity{};
  Vec3 capture_point{};
  Vec3 support_center{};
  u32 support_count = 0;
  Vec3 foot_target[kFootCount]{};
  Vec3 swing_position[kFootCount]{};
  StepReject step_reject[kFootCount]{};
  f32 gait_phase = 0;
  f32 max_torque_saturation = 0;  // worst joint |applied|/budget last tick
  ControlMode mode = ControlMode::kStable;
  f32 mode_time = 0;
};

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_TYPES_H_
