// rx::locomotion whole-body target generation (docs/LOCOMOTION.md §whole_body).
// Analytic 2-bone leg IK, torso/head orientation from facing + ground normal +
// acceleration lean, gait-phase arm counter-swing, per-group drive scales and
// two bounded root assists. Pure deterministic math; every path finite.

#include "locomotion/whole_body.h"

#include <cmath>

#include "locomotion/internal_math.h"

namespace rx::locomotion {
using namespace internal;

namespace {

constexpr f32 kPi = 3.14159265358979f;

// Clamp a rotation to a maximum cone angle (radians), shortest-arc.
Quat ClampCone(Quat q, f32 max_angle) {
  q = Normalize(q);
  if (q.w < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};  // pick the shortest hemisphere
  f32 angle = 2.0f * std::acos(Clampf(q.w, -1.0f, 1.0f));
  if (angle <= max_angle) return q;
  Vec3 axis{q.x, q.y, q.z};
  if (Length(axis) < 1e-6f) return {0, 0, 0, 1};
  return QuatFromAxisAngle(axis, max_angle);
}

// Scale a rotation about its own axis by `t` (Slerp from identity).
Quat ScaleRotation(const Quat& q, f32 t) { return Slerp({0, 0, 0, 1}, q, t); }

// Yaw-only rotation taking the bind forward (-Z) onto a planar facing `f`.
Quat YawFromFacing(const Vec3& f) { return QuatBetween({0, 0, -1}, f); }

}  // namespace

LegIkResult SolveLegIk(const Vec3& hip_to_sole, const Vec3& sole_normal_parent, f32 upper_length,
                       f32 lower_length, f32 foot_height) {
  LegIkResult r;
  const Vec3 up_y{0, 1, 0};
  const Vec3 down_y{0, -1, 0};
  const Vec3 knee_axis{-1, 0, 0};  // -X for both legs; positive flexion swings +Z

  // Degenerate / non-finite target -> bind-ish, flagged.
  if (!FiniteV(hip_to_sole) || Length(hip_to_sole) < 1e-5f) {
    r.hip = {0, 0, 0, 1};
    r.knee_flexion = 0.0f;
    r.ankle = {0, 0, 0, 1};
    r.clamped = true;
    return r;
  }

  // Desired sole normal (guard zero / non-finite -> +Y).
  Vec3 n = sole_normal_parent;
  if (!FiniteV(n) || Length(n) < 1e-6f)
    n = up_y;
  else
    n = Normalize(n);

  // The ankle joint sits foot_height above the sole along the sole normal.
  const Vec3 ankle_target = hip_to_sole + n * foot_height;
  const f32 raw = Length(ankle_target);
  if (!FiniteScalar(raw) || raw < 1e-5f) {
    r.hip = {0, 0, 0, 1};
    r.knee_flexion = 0.0f;
    r.ankle = {0, 0, 0, 1};
    r.clamped = true;
    return r;
  }

  const f32 u = upper_length;
  const f32 l = lower_length;
  const f32 eps = 1e-4f;
  const f32 d_min = std::fabs(u - l) + eps;
  const f32 d_max = u + l - eps;

  f32 d = raw;
  if (d < d_min) {
    d = d_min;
    r.clamped = true;
  }
  if (d > d_max) {
    d = d_max;
    r.clamped = true;
  }

  const Vec3 a_hat = ankle_target * (1.0f / raw);  // unit hip->ankle direction

  // Knee flexion from the law of cosines. Straight leg (d = u+l) -> 0, fully
  // folded (d = |u-l|) -> pi. Never negative.
  f32 cos_knee = (u * u + l * l - d * d) / (2.0f * u * l);
  cos_knee = Clampf(cos_knee, -1.0f, 1.0f);
  r.knee_flexion = kPi - std::acos(cos_knee);
  if (r.knee_flexion < 0.0f) r.knee_flexion = 0.0f;

  // Triangle angle at the hip between the thigh (bind -Y) and the hip->ankle
  // line. In the bent, un-aimed configuration the ankle lies at angle `alpha`
  // off -Y in the knee's hinge plane (about -X). Pre-rotating the thigh by
  // -alpha about the knee axis and then swinging -Y onto the ankle direction
  // lands the *bent* ankle exactly on the target:
  //   hip = QuatBetween(-Y, a_hat) * QuatFromAxisAngle(-X, -alpha)
  // FK-verified (test/locomotion_math_test.cc): Rotate(hip, V0) == ankle_target
  // where V0 is the bent chain's ankle in the pre-hip frame.
  f32 cos_alpha = (u * u + d * d - l * l) / (2.0f * u * d);
  cos_alpha = Clampf(cos_alpha, -1.0f, 1.0f);
  const f32 alpha = std::acos(cos_alpha);
  r.hip = QuatBetween(down_y, a_hat) * QuatFromAxisAngle(knee_axis, -alpha);

  // Ankle: after hip+knee, rotate the foot's bind normal (+Y) onto n. Solve in
  // the parent (lower-leg) frame; QuatBetween is shortest-arc so no shank twist
  // is introduced. Clamp to a 0.5 rad cone.
  const Quat chain = r.hip * QuatFromAxisAngle(knee_axis, r.knee_flexion);
  const Vec3 n_parent = Rotate(Conjugate(chain), n);
  r.ankle = ClampCone(QuatBetween(up_y, n_parent), 0.5f);

  if (!FiniteQ(r.hip)) r.hip = {0, 0, 0, 1};
  if (!FiniteQ(r.ankle)) r.ankle = {0, 0, 0, 1};
  if (!FiniteScalar(r.knee_flexion)) r.knee_flexion = 0.0f;
  return r;
}

void BuildWholeBodyTargets(const CharacterMeasurements& m, const ContactEstimate& contacts,
                           const GaitState& gait, const FootPlan plan[kFootCount],
                           const LocomotionIntent& intent, const PhysicalModifiers& modifiers,
                           const ControllerParameters& params, const BipedRig& rig,
                           WholeBodyTargets* out) {
  const Vec3 up_y{0, 1, 0};
  const f32 gravity = m.gravity;
  const f32 strength = FiniteScalar(modifiers.strength) ? modifiers.strength : 1.0f;
  const f32 balance = FiniteScalar(modifiers.balance) ? modifiers.balance : 1.0f;

  // --- 1. Frames ----------------------------------------------------------
  Quat q_p = FiniteQ(m.root_rotation) ? Normalize(m.root_rotation) : Quat{0, 0, 0, 1};

  Vec3 f = Planar(intent.desired_facing);
  if (Length(f) < 1e-4f) f = Planar(Rotate(q_p, {0, 0, -1}));  // pelvis forward
  if (Length(f) < 1e-4f) f = {0, 0, -1};
  f = Normalize(f);

  const Quat q_yaw = YawFromFacing(f);
  const Vec3 right = Rotate(q_yaw, {1, 0, 0});  // character-right after yaw

  // --- 2. Desired pelvis pose (the IK frame) ------------------------------
  Vec3 support_center =
      FiniteV(contacts.support_center) ? contacts.support_center : m.root_position;
  // The IK frame sits at the MEASURED pelvis planar position. Averaging it toward
  // support_center (as an earlier revision did) biases the frame by the foot-box
  // forward offset the estimator reports as the sole, and since no horizontal
  // pelvis-position force enforces that target, the leg IK resolves the mismatch
  // by walking the real pelvis toward it — a standing forward creep. Balance is
  // instead handled by the capture-point footstep planner and the planar
  // velocity assist below, which do not fight a stationary stance. (Both math
  // tests place the root and support_center at the origin, so this term is
  // exercised only in the integrated controller.)
  const Vec3 planar_pos = Planar(m.root_position);

  f32 pelvis_h = rig.pelvis_height;
  if (intent.desired_body_height > 0.0f && params.body_height > 0.0f)
    pelvis_h = rig.pelvis_height * (intent.desired_body_height / params.body_height);
  if (intent.request_crouch) pelvis_h -= 0.25f * rig.leg_length;

  const f32 desired_pelvis_y = support_center.y + pelvis_h;
  const Vec3 pelvis_pos{planar_pos.x, desired_pelvis_y, planar_pos.z};

  // Acceleration lean about the pelvis-right axis, plus a partial ground tilt.
  Vec3 a_des = Planar(intent.desired_velocity) - Planar(m.com_velocity);
  a_des = ClampLength(a_des, params.max_acceleration);
  const f32 lean_angle = Clampf(-0.03f * Dot(a_des, f), -0.20f, 0.20f);  // k_lean s^2/m

  Quat q_d = QuatFromAxisAngle(right, lean_angle) * q_yaw;
  Vec3 ground_normal = FiniteV(m.ground_normal) && Length(m.ground_normal) > 1e-6f
                           ? Normalize(m.ground_normal)
                           : up_y;
  q_d = Slerp(q_d, QuatBetween(up_y, ground_normal) * q_d, 0.30f);  // 30% ground tilt
  if (!FiniteQ(q_d)) q_d = q_yaw;
  const Quat q_d_inv = Conjugate(q_d);

  // --- 3. Legs ------------------------------------------------------------
  const RigJoint hip_joint[kFootCount] = {RigJoint::kHipL, RigJoint::kHipR};
  const RigJoint knee_joint[kFootCount] = {RigJoint::kKneeL, RigJoint::kKneeR};
  const RigJoint ankle_joint[kFootCount] = {RigJoint::kAnkleL, RigJoint::kAnkleR};

  for (u32 i = 0; i < kFootCount; ++i) {
    const FootPlan& p = plan[i];
    const Vec3 sole_world = p.swinging ? p.swing_position : p.target;
    const Vec3 hip_world = pelvis_pos + Rotate(q_d, rig.hip_local[i]);
    const Vec3 hip_to_sole = Rotate(q_d_inv, sole_world - hip_world);
    const Quat desired_foot_parent = Normalize(q_d_inv * p.foot_orientation);
    const Vec3 sole_normal_parent = Rotate(desired_foot_parent, up_y);

    const LegIkResult leg = SolveLegIk(hip_to_sole, sole_normal_parent, rig.upper_leg_length,
                                       rig.lower_leg_length, rig.foot_height);
    const Quat knee = QuatFromAxisAngle({-1, 0, 0}, leg.knee_flexion);
    Quat ankle = leg.ankle;
    const Quat solved_foot_parent = leg.hip * knee * ankle;
    Vec3 solved_forward = Planar(Rotate(solved_foot_parent, {0, 0, -1}));
    Vec3 desired_forward = Planar(Rotate(desired_foot_parent, {0, 0, -1}));
    if (Length(solved_forward) > 1e-4f && Length(desired_forward) > 1e-4f) {
      solved_forward = Normalize(solved_forward);
      desired_forward = Normalize(desired_forward);
      const f32 yaw_error =
          std::atan2(Cross(solved_forward, desired_forward).y,
                     Clampf(Dot(solved_forward, desired_forward), -1.0f, 1.0f));
      ankle = ClampCone(ankle * QuatFromAxisAngle(up_y, yaw_error), 0.5f);
    }

    out->joint_target[static_cast<u32>(hip_joint[i])] = leg.hip;
    out->joint_target[static_cast<u32>(knee_joint[i])] = knee;
    out->joint_target[static_cast<u32>(ankle_joint[i])] = ankle;

    const f32 leg_scale = p.swinging ? 0.75f : 1.0f;
    out->joint_drive_scale[static_cast<u32>(hip_joint[i])] = leg_scale * strength;
    out->joint_drive_scale[static_cast<u32>(knee_joint[i])] = leg_scale * strength;
    out->joint_drive_scale[static_cast<u32>(ankle_joint[i])] = leg_scale * strength;
  }

  // --- 4. Waist + neck ----------------------------------------------------
  // Torso faces f and stays upright while the pelvis leans: aim it at the yaw
  // plus half the acceleration lean, then express the delta in the pelvis frame.
  const Quat torso_world = QuatFromAxisAngle(right, 0.5f * lean_angle) * q_yaw;
  Quat waist = ClampCone(q_d_inv * torso_world, 0.4f);
  // Neck counters half of the waist so the head stays level.
  Quat neck = ClampCone(ScaleRotation(Conjugate(waist), 0.5f), 0.4f);
  out->joint_target[static_cast<u32>(RigJoint::kWaist)] = waist;
  out->joint_target[static_cast<u32>(RigJoint::kNeck)] = neck;
  out->joint_drive_scale[static_cast<u32>(RigJoint::kWaist)] = 0.9f * strength;
  out->joint_drive_scale[static_cast<u32>(RigJoint::kNeck)] = 0.4f * strength;

  // --- 5. Arms: gait-phase counter-swing (arm opposes the same-side leg) ---
  const f32 speed_ratio = Clampf(gait.speed_ratio, 0.0f, 4.0f);
  const f32 swing_l = 0.45f * speed_ratio * std::cos(2.0f * kPi * GaitClock::FootPhase(gait, 1));
  const f32 swing_r = 0.45f * speed_ratio * std::cos(2.0f * kPi * GaitClock::FootPhase(gait, 0));
  const f32 elbow_flex = 0.25f + 0.25f * speed_ratio;
  out->joint_target[static_cast<u32>(RigJoint::kShoulderL)] = QuatFromAxisAngle({1, 0, 0}, swing_l);
  out->joint_target[static_cast<u32>(RigJoint::kShoulderR)] = QuatFromAxisAngle({1, 0, 0}, swing_r);
  out->joint_target[static_cast<u32>(RigJoint::kElbowL)] = QuatFromAxisAngle({1, 0, 0}, elbow_flex);
  out->joint_target[static_cast<u32>(RigJoint::kElbowR)] = QuatFromAxisAngle({1, 0, 0}, elbow_flex);
  const f32 arm_scale = params.arm_drive_scale * strength;
  out->joint_drive_scale[static_cast<u32>(RigJoint::kShoulderL)] = arm_scale;
  out->joint_drive_scale[static_cast<u32>(RigJoint::kShoulderR)] = arm_scale;
  out->joint_drive_scale[static_cast<u32>(RigJoint::kElbowL)] = arm_scale;
  out->joint_drive_scale[static_cast<u32>(RigJoint::kElbowR)] = arm_scale;

  // Any non-finite joint target falls back to bind.
  for (u32 j = 0; j < kRigJointCount; ++j) {
    if (!FiniteQ(out->joint_target[j])) out->joint_target[j] = {0, 0, 0, 1};
    if (!FiniteScalar(out->joint_drive_scale[j])) out->joint_drive_scale[j] = 0.0f;
  }

  // --- 7. Root assists (bounded cheats) -----------------------------------
  const f32 mass =
      params.total_mass + (FiniteScalar(modifiers.carried_mass) ? modifiers.carried_mass : 0.0f);
  Vec3 assist_force{0, 0, 0};
  Vec3 assist_torque{0, 0, 0};
  if (contacts.support_count > 0) {
    const f32 height_error = desired_pelvis_y - m.root_position.y;
    f32 f_y = mass * (params.pelvis_position_gain * height_error +
                      params.pelvis_velocity_gain * (-m.com_velocity.y));
    const f32 f_y_cap = 0.45f * mass * gravity;
    f_y = Clampf(f_y, -f_y_cap, f_y_cap);

    Vec3 f_planar = (Planar(intent.desired_velocity) - Planar(m.com_velocity)) * (mass * 1.1f);
    f_planar = ClampLength(f_planar, 0.15f * mass * gravity);
    assist_force = f_planar + up_y * f_y;

    // Upright torque: rotate the pelvis up axis onto the DESIRED trunk up (from
    // q_d), not pure vertical. Righting strictly to vertical would cancel the
    // acceleration lean that drives inverted-pendulum walking, leaving only a
    // pelvis-force shove that pitches the body over its feet. Targeting the
    // desired (leaned) up keeps the intended forward lean while still resisting
    // any excess tilt beyond it. (Standing: q_d is upright, so this reduces to
    // the vertical target.)
    const Vec3 up_world = Rotate(q_p, up_y);
    const Vec3 up_target = Rotate(q_d, up_y);
    const Vec3 cross = Cross(up_world, up_target);
    const f32 sin_a = Length(cross);
    const f32 angle = std::acos(Clampf(Dot(up_world, up_target), -1.0f, 1.0f));
    const Vec3 axis = sin_a > 1e-6f ? cross * (1.0f / sin_a) : Vec3{0, 0, 0};
    assist_torque = axis * (params.torso_orientation_gain * angle) -
                    m.root_angular_velocity * params.torso_angular_damping;
    assist_torque = ClampLength(assist_torque, params.root_assist_torque * balance);
  }
  if (!FiniteV(assist_force)) assist_force = {0, 0, 0};
  if (!FiniteV(assist_torque)) assist_torque = {0, 0, 0};
  out->root_assist_force = assist_force;
  out->root_assist_torque = assist_torque;

  // --- 8. Copy the plans through --------------------------------------------
  for (u32 i = 0; i < kFootCount; ++i) out->foot[i] = plan[i];
}

}  // namespace rx::locomotion
