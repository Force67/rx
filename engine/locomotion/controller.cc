// rx::locomotion LocomotionController: the fixed-update orchestration of
// docs/LOCOMOTION.md. Runs BEFORE physics::PhysicsWorld::Update each fixed step
// and closes the feedback loop
//
//   measure -> classify contacts -> mode machine -> gait -> footstep plan
//           -> whole-body targets -> joint motor commands + bounded root assist.
//
// Nothing here teleports a body: all actuation is joint motor targets/limits
// plus the small root force/torque assists. No per-tick heap allocation.
//
// The five control modes are a physical-regime state machine only. Motors are
// never cut instantly; drive_blend_ eases the motor-strength budget across
// transitions with time constant params.recovery_blend_time so a failing
// controller sags rather than snapping limp.
//
// Recovery (kRecovering) is deliberately modest (docs/LOCOMOTION.md scopes the
// full get-up choreography out): it blends drive back up, folds the legs into a
// loose crouch and rights the torso with a bounded upright torque. It promotes
// to kStable if the body actually comes back up within a few seconds, otherwise
// it drops back to kGrounded and retries while intent.allow_recovery holds. It
// is not a hand/knee-staged stand-up.

#include "locomotion/controller.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "locomotion/whole_body.h"
#include "physics/physics_world.h"

namespace rx::locomotion {
namespace {

constexpr f32 kGravity = 9.81f;

bool FiniteScalar(f32 x) { return std::isfinite(x); }
bool FiniteV(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool FiniteQ(const Quat& q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

f32 Clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : (x > hi ? hi : x); }

Vec3 Planar(const Vec3& v) { return {v.x, 0.0f, v.z}; }
f32 PlanarLength(const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }

Vec3 ClampLength(const Vec3& v, f32 max_len) {
  const f32 len = Length(v);
  if (len <= max_len || len <= 0) return v;
  return v * (max_len / len);
}

// Exponential smoothing coefficient for a first-order lag (tau seconds) over dt.
f32 SmoothingAlpha(f32 dt, f32 tau) {
  if (!(dt > 0) || !(tau > 0)) return 0;
  return 1.0f - std::exp(-dt / tau);
}

// Angle (rad) between two directions, both assumed roughly unit-length.
f32 AngleBetween(const Vec3& a, const Vec3& b) {
  return std::acos(Clampf(Dot(a, b), -1.0f, 1.0f));
}

}  // namespace

LocomotionController::~LocomotionController() { Destroy(); }

bool LocomotionController::GroundProbe(void* context, const Vec3& probe_start, f32 max_depth,
                                       GroundHit* out) {
  auto* self = static_cast<LocomotionController*>(context);
  if (!self || !self->physics_) return false;
  physics::PhysicsWorld::RayHit hit;
  if (!self->physics_->Raycast(probe_start, {0, -1, 0}, max_depth, &hit)) return false;
  out->position = hit.position;
  out->normal = hit.normal;
  return true;
}

bool LocomotionController::IsRigBody(physics::BodyId id) const {
  if (id == 0) return false;
  for (u32 i = 0; i < kBodyPartCount; ++i)
    if (rig_.body[i] == id) return true;
  return false;
}

Vec3 LocomotionController::AnkleWorld(u32 foot) const {
  const u32 idx = static_cast<u32>(foot == 0 ? BodyPart::kFootL : BodyPart::kFootR);
  Vec3 p;
  f32 r[4] = {0, 0, 0, 1};
  if (!physics_->GetBodyTransform(rig_.body[idx], &p, r)) return p;
  const Quat q{r[0], r[1], r[2], r[3]};
  // Foot box centre -> ankle pivot: up by sole_offset (= foot_h/2) and back by
  // the box's forward offset (0.2 * foot_length; the box is centred 0.2*len
  // forward of the ankle in rig.cc).
  const Vec3 local{0, rig_.sole_offset, 0.2f * rig_.foot_length};
  return p + Rotate(q, local);
}

bool LocomotionController::HasEnvironmentContact(physics::BodyId watched) const {
  physics::PhysicsWorld::BodyContact contacts[8];
  const u32 n = physics_->GetBodyContacts(watched, contacts, 8);
  for (u32 c = 0; c < n; ++c) {
    // `other == 0` means the counterpart could not be identified (the static
    // floor is not one of our handles); either way it is the environment, not
    // another rig limb.
    if (!IsRigBody(contacts[c].other)) return true;
  }
  return false;
}

bool LocomotionController::Initialize(physics::PhysicsWorld* physics,
                                      const ControllerParameters& params, const Vec3& feet_position,
                                      f32 yaw) {
  Destroy();
  physics_ = physics;
  params_ = params;
  if (!physics_) return false;
  if (!BipedRig::Build(*physics_, params_, feet_position, yaw, &rig_)) {
    physics_ = nullptr;
    return false;
  }

  state_estimator_ = StateEstimator{};
  contact_estimator_.Reset();
  gait_clock_.Reset();
  footstep_planner_.Reset();
  measurements_ = CharacterMeasurements{};
  contacts_ = ContactEstimate{};
  gait_ = GaitState{};
  targets_ = WholeBodyTargets{};
  debug_ = DebugState{};

  mode_ = ControlMode::kStable;
  mode_time_ = 0;
  drive_blend_ = 1;
  tick_count_ = 0;
  for (u32 j = 0; j < kRigJointCount; ++j) applied_torque_[j] = 0;
  prev_tilt_ = 0;
  fall_ticks_ = 0;
  tilt_over_time_ = 0;
  no_support_time_ = 0;
  grounded_low_time_ = 0;
  return true;
}

void LocomotionController::Destroy() {
  if (physics_ && rig_.valid()) rig_.Destroy(*physics_);
  rig_ = BipedRig{};
  physics_ = nullptr;
}

void LocomotionController::Tick(const LocomotionIntent& intent, const PhysicalModifiers& modifiers,
                                f32 dt) {
  if (!initialized() || !(dt > 0)) return;

  const physics::BodyId pelvis = rig_.body[static_cast<u32>(BodyPart::kPelvis)];
  const physics::BodyId torso = rig_.body[static_cast<u32>(BodyPart::kTorso)];

  // Step 2 (kept early): the powered ragdoll island must never sleep mid-control
  // or the estimators lose their live contact callbacks. ApplyForce activates
  // unconditionally, so a zero force keeps the pelvis (and its constraint
  // island) awake even on the safety-stop path below.
  physics_->ApplyForce(pelvis, Vec3{});

  const f32 blend_alpha = SmoothingAlpha(dt, params_.recovery_blend_time);
  const f32 strength = FiniteScalar(modifiers.strength) ? modifiers.strength : 1.0f;
  const f32 balance = (FiniteScalar(modifiers.balance) && modifiers.balance > 0) ? modifiers.balance
                                                                                 : 1.0f;

  // --- Step 1: MEASURE ----------------------------------------------------
  state_estimator_.Measure(*physics_, rig_, modifiers, &measurements_);
  const CharacterMeasurements& m = measurements_;

  if (!m.valid) {
    // Safety stop: ease every joint's motor budget toward (near) zero over
    // recovery_blend_time and drop to a controlled fall; hold whatever pose the
    // joints are in. Never snap to a limp ragdoll.
    drive_blend_ += (0.05f - drive_blend_) * blend_alpha;
    for (u32 j = 0; j < kRigJointCount; ++j) {
      const RigJoint rj = static_cast<RigJoint>(j);
      const f32 torque = params_.max_joint_torque * drive_blend_;
      rig_.SetJointDrive(*physics_, rj, params_.joint_frequency, params_.joint_damping, torque);
      applied_torque_[j] = torque;
    }
    if (mode_ != ControlMode::kControlledFall) {
      mode_ = ControlMode::kControlledFall;
      mode_time_ = 0;
    } else {
      mode_time_ += dt;
    }
    debug_ = DebugState{};
    debug_.desired_velocity = intent.desired_velocity;
    debug_.mode = mode_;
    debug_.mode_time = mode_time_;
    return;
  }

  contact_estimator_.Update(m, dt);
  contacts_ = contact_estimator_.estimate();

  // --- Step 3: MODE MACHINE (from measured physics only) ------------------
  const f32 com_height = Clampf(m.com_position.y - contacts_.support_center.y, 0.3f, 1e4f);
  const Vec3 cp = CapturePoint(m.com_position, m.com_velocity, kGravity, com_height);
  // Balance error = how far the capture point strays from where a body moving at
  // the COMMANDED velocity should carry it. A steady walk keeps the capture point
  // a lead of ~desired_velocity/omega ahead of the support (that IS forward
  // locomotion, not a fall); subtracting that lead keeps `margin` near zero while
  // walking and only grows on genuine, uncommanded imbalance.
  const f32 omega = std::sqrt(kGravity / com_height);
  const Vec3 expected_lead = Planar(intent.desired_velocity) * (1.0f / omega);
  const f32 margin =
      PlanarLength(Planar(cp) - Planar(contacts_.support_center) - expected_lead);

  const Vec3 up_world = FiniteQ(m.root_rotation) ? Rotate(m.root_rotation, Vec3{0, 1, 0})
                                                 : Vec3{0, 1, 0};
  const f32 tilt = AngleBetween(up_world, Vec3{0, 1, 0});
  const bool tilt_rising = tilt > prev_tilt_ + 1e-4f;

  const f32 speed = Length(m.com_velocity);
  const f32 margin_thresh = params_.recovery_margin * balance;

  // Fall timers.
  if (tilt >= params_.fall_pitch_limit)
    tilt_over_time_ += dt;
  else
    tilt_over_time_ = 0;
  if (contacts_.support_count == 0)
    no_support_time_ += dt;
  else
    no_support_time_ = 0;

  const bool fall_condition =
      (margin >= 2.5f * params_.recovery_margin && tilt_rising) || (tilt_over_time_ > 0.1f) ||
      (no_support_time_ > 0.5f && speed > 1.0f);

  ControlMode next = mode_;
  switch (mode_) {
    case ControlMode::kStable:
    case ControlMode::kCorrectiveStep: {
      if (fall_condition) {
        ++fall_ticks_;
        if (fall_ticks_ >= 2) next = ControlMode::kControlledFall;  // 2-tick hysteresis
      } else {
        fall_ticks_ = 0;
        const bool corrective = margin > margin_thresh &&
                                margin < 2.5f * params_.recovery_margin &&
                                tilt < params_.fall_pitch_limit && contacts_.support_count >= 1;
        const bool stable =
            margin <= margin_thresh && tilt < 0.6f * params_.fall_pitch_limit;
        if (stable)
          next = ControlMode::kStable;
        else if (corrective)
          next = ControlMode::kCorrectiveStep;
        // else: ambiguous band -> hold the current mode.
      }
      break;
    }
    case ControlMode::kControlledFall: {
      const bool env_contact =
          HasEnvironmentContact(pelvis) || HasEnvironmentContact(torso);
      const bool low_com =
          m.com_position.y <
          params_.grounded_height_fraction * rig_.leg_length + contacts_.support_center.y;
      const bool slow = speed < 0.35f;
      if (env_contact && low_com && slow)
        grounded_low_time_ += dt;
      else
        grounded_low_time_ = 0;
      if (grounded_low_time_ >= params_.grounded_dwell_time) next = ControlMode::kGrounded;
      break;
    }
    case ControlMode::kGrounded: {
      if (intent.allow_recovery) next = ControlMode::kRecovering;
      break;
    }
    case ControlMode::kRecovering: {
      const bool recovered =
          tilt < 0.5f &&
          m.com_position.y > 0.7f * rig_.pelvis_height + contacts_.support_center.y;
      if (recovered)
        next = ControlMode::kStable;
      else if (!intent.allow_recovery || mode_time_ > 3.0f)
        next = ControlMode::kGrounded;  // give up this attempt, retry from grounded
      break;
    }
  }

  if (next != mode_) {
    mode_ = next;
    mode_time_ = 0;
    fall_ticks_ = 0;
    tilt_over_time_ = 0;
    no_support_time_ = 0;
    grounded_low_time_ = 0;
  } else {
    mode_time_ += dt;
  }
  prev_tilt_ = tilt;

  // drive_blend_ eases toward the mode's motor-strength target (never 0).
  f32 target_blend = 1.0f;
  switch (mode_) {
    case ControlMode::kStable:
    case ControlMode::kCorrectiveStep:
      target_blend = 1.0f;
      break;
    case ControlMode::kControlledFall:
      target_blend = 0.25f;
      break;
    case ControlMode::kGrounded:
      target_blend = 0.15f;
      break;
    case ControlMode::kRecovering:
      target_blend = 0.6f;
      break;
  }
  drive_blend_ += (target_blend - drive_blend_) * blend_alpha;

  // --- Step 4/5: build the tick's whole-body targets ----------------------
  // Populate targets_ fully for every mode, then a common actuation pass drives
  // the motors. Root force/torque distribution stays mode-specific.
  const bool control_mode =
      mode_ == ControlMode::kStable || mode_ == ControlMode::kCorrectiveStep;

  if (control_mode) {
    const bool need_step = mode_ == ControlMode::kCorrectiveStep;
    gait_clock_.Update(m, intent, params_, need_step, dt);
    gait_ = gait_clock_.state();
    footstep_planner_.Update(m, contacts_, gait_, intent, params_, &GroundProbe, this, dt);
    const FootPlan plan[kFootCount] = {footstep_planner_.plan(0), footstep_planner_.plan(1)};
    BuildWholeBodyTargets(m, contacts_, gait_, plan, intent, modifiers, params_, rig_, &targets_);

    // Ankle-strategy balance assist. The whole-body root force only DAMPS planar
    // velocity; a damper alone lets the COM drift off the base and topple. Anchor
    // the COM over the mean ANKLE pivot (the true standing base — the estimator's
    // foot-box "sole" sits forward of it), decomposed relative to the commanded
    // travel direction:
    //   * the LATERAL (perpendicular) component is always full strength — without
    //     it a walk has no side-to-side foot-placement balance and simply veers
    //     over and topples;
    //   * the FORWARD (parallel) component fades out with commanded speed so it
    //     never fights a deliberate walk (forward motion is tracked by the
    //     whole-body velocity assist and caught by footstep placement instead).
    // Bounded like the other assists; only while at least one foot supports.
    if (contacts_.support_count >= 1) {
      const f32 mass = params_.total_mass +
                       (FiniteScalar(modifiers.carried_mass) ? modifiers.carried_mass : 0.0f);
      const Vec3 base = (AnkleWorld(0) + AnkleWorld(1)) * 0.5f;
      const Vec3 offset = Planar(base) - Planar(m.com_position);  // toward the base
      const Vec3 com_v = Planar(m.com_velocity);

      const f32 desired_speed = PlanarLength(intent.desired_velocity);
      Vec3 fdir = desired_speed > 0.05f ? Planar(intent.desired_velocity)
                                        : Planar(Rotate(m.root_rotation, Vec3{0, 0, -1}));
      fdir = Length(fdir) > 1e-4f ? Normalize(fdir) : Vec3{0, 0, -1};
      const Vec3 perp = Normalize(Cross(Vec3{0, 1, 0}, fdir));  // horizontal, side axis
      const f32 fade = Clampf((0.3f - desired_speed) / 0.3f, 0.0f, 1.0f);

      const f32 lat_off = Dot(offset, perp);
      const f32 lat_vel = Dot(com_v, perp);
      const f32 fwd_off = Dot(offset, fdir);
      const f32 fwd_vel = Dot(com_v, fdir);
      Vec3 balance = perp * (mass * (45.0f * lat_off - 8.0f * lat_vel)) +
                     fdir * (mass * fade * (45.0f * fwd_off - 6.0f * fwd_vel));
      balance = ClampLength(balance, 0.35f * mass * kGravity);
      targets_.root_assist_force += balance;
    }
  } else {
    // Non-control regimes: build simple pose targets by hand and freeze the
    // foot plans onto the measured feet (for debug continuity).
    for (u32 j = 0; j < kRigJointCount; ++j) {
      targets_.joint_target[j] = Quat{0, 0, 0, 1};
      targets_.joint_drive_scale[j] = 0;
    }
    targets_.root_assist_force = Vec3{};
    targets_.root_assist_torque = Vec3{};
    for (u32 f = 0; f < kFootCount; ++f) {
      targets_.foot[f] = FootPlan{};
      targets_.foot[f].target = m.foot[f].position;
      targets_.foot[f].swing_position = m.foot[f].position;
      targets_.foot[f].foot_orientation = Quat{0, 0, 0, 1};
    }

    if (mode_ == ControlMode::kControlledFall) {
      // Legs to a mild protective tuck, arms loose, neck holds the head up.
      const Quat hip_tuck = QuatFromAxisAngle({1, 0, 0}, 0.4f);   // thigh forward
      const Quat knee_tuck = QuatFromAxisAngle({-1, 0, 0}, 0.9f); // flexion
      targets_.joint_target[static_cast<u32>(RigJoint::kHipL)] = hip_tuck;
      targets_.joint_target[static_cast<u32>(RigJoint::kHipR)] = hip_tuck;
      targets_.joint_target[static_cast<u32>(RigJoint::kKneeL)] = knee_tuck;
      targets_.joint_target[static_cast<u32>(RigJoint::kKneeR)] = knee_tuck;
      const f32 leg_scale = 1.0f * strength;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kHipL)] = leg_scale;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kHipR)] = leg_scale;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kKneeL)] = leg_scale;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kKneeR)] = leg_scale;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kAnkleL)] = 0.5f * strength;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kAnkleR)] = 0.5f * strength;
      // Neck protects the head.
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kNeck)] = 0.5f * strength;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kWaist)] = 0.4f * strength;
      // Arms loose.
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kShoulderL)] = 0.15f * strength;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kShoulderR)] = 0.15f * strength;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kElbowL)] = 0.15f * strength;
      targets_.joint_drive_scale[static_cast<u32>(RigJoint::kElbowR)] = 0.15f * strength;
    } else if (mode_ == ControlMode::kGrounded) {
      // Hold current-ish pose (identity deltas) at low drive.
      for (u32 j = 0; j < kRigJointCount; ++j) targets_.joint_drive_scale[j] = 1.0f * strength;
    } else {  // kRecovering
      const Quat hip_crouch = QuatFromAxisAngle({1, 0, 0}, 0.9f);
      const Quat knee_crouch = QuatFromAxisAngle({-1, 0, 0}, 1.6f);
      targets_.joint_target[static_cast<u32>(RigJoint::kHipL)] = hip_crouch;
      targets_.joint_target[static_cast<u32>(RigJoint::kHipR)] = hip_crouch;
      targets_.joint_target[static_cast<u32>(RigJoint::kKneeL)] = knee_crouch;
      targets_.joint_target[static_cast<u32>(RigJoint::kKneeR)] = knee_crouch;
      // Waist upright (bind is upright); the upright torso torque below does the
      // real righting work.
      for (u32 j = 0; j < kRigJointCount; ++j) targets_.joint_drive_scale[j] = 1.0f * strength;
    }
  }

  // --- Step 7: finite guard over everything we are about to apply ----------
  bool all_finite = FiniteV(targets_.root_assist_force) && FiniteV(targets_.root_assist_torque);
  for (u32 j = 0; j < kRigJointCount && all_finite; ++j)
    all_finite = FiniteQ(targets_.joint_target[j]) && FiniteScalar(targets_.joint_drive_scale[j]);

  if (all_finite) {
    // Joint motors, common to every mode. SetJointTarget is cheap (one motor
    // write); SetJointDrive walks Jolt constraint settings, so re-issue it only
    // when the effective torque budget moved > 1%.
    for (u32 j = 0; j < kRigJointCount; ++j) {
      const RigJoint rj = static_cast<RigJoint>(j);
      rig_.SetJointTarget(*physics_, rj, targets_.joint_target[j]);
      const f32 torque = params_.max_joint_torque * targets_.joint_drive_scale[j] * drive_blend_;
      const f32 ref = applied_torque_[j] > 1e-3f ? applied_torque_[j] : 1.0f;
      if (std::fabs(torque - applied_torque_[j]) > 0.01f * ref) {
        rig_.SetJointDrive(*physics_, rj, params_.joint_frequency, params_.joint_damping, torque);
        applied_torque_[j] = torque;
      }
    }

    // Root assists, distributed per mode.
    if (control_mode) {
      const Vec3 assist_force = targets_.root_assist_force * drive_blend_;
      physics_->ApplyForce(pelvis, assist_force);
      // Upright assist goes to the TORSO (righting the mass that matters) with an
      // equal-and-opposite -0.5x on the pelvis, so the couple is closer to an
      // internal effort than an external shove. Both scale with drive_blend_.
      const Vec3 upright = targets_.root_assist_torque * drive_blend_;
      physics_->ApplyTorque(torso, upright);
      physics_->ApplyTorque(pelvis, upright * -0.5f);
    } else if (mode_ == ControlMode::kControlledFall) {
      // Bleed extreme spin only: a torque opposing the measured angular velocity,
      // clamped small so it can never inject energy. No root force assist.
      Vec3 damp = m.root_angular_velocity * (-params_.torso_angular_damping);
      damp = ClampLength(damp, 0.25f * params_.root_assist_torque);
      physics_->ApplyTorque(pelvis, damp * drive_blend_);
    } else if (mode_ == ControlMode::kRecovering) {
      // Modest upright torso torque, same couple split as the control path.
      const Vec3 cross = Cross(up_world, Vec3{0, 1, 0});
      const f32 sin_a = Length(cross);
      const f32 angle = AngleBetween(up_world, Vec3{0, 1, 0});
      const Vec3 axis = sin_a > 1e-6f ? cross * (1.0f / sin_a) : Vec3{0, 0, 0};
      Vec3 upright = axis * (params_.torso_orientation_gain * angle) -
                     m.root_angular_velocity * params_.torso_angular_damping;
      upright = ClampLength(upright, 0.5f * params_.root_assist_torque);
      physics_->ApplyTorque(torso, upright * drive_blend_);
      physics_->ApplyTorque(pelvis, upright * (-0.5f * drive_blend_));
    }
    // kGrounded: joint motors only, no root assist.
  } else {
    // Non-finite anywhere: skip actuation this tick and flag the measurement so
    // next tick takes the safety-stop path.
    measurements_.valid = false;
  }

  // --- Step 6: DEBUG (filled completely every tick) -----------------------
  debug_.desired_velocity = intent.desired_velocity;
  debug_.measured_velocity = m.com_velocity;
  debug_.com_position = m.com_position;
  debug_.com_velocity = m.com_velocity;
  debug_.capture_point = cp;
  debug_.support_center = contacts_.support_center;
  debug_.support_count = contacts_.support_count;
  f32 max_commanded = 0;
  for (u32 f = 0; f < kFootCount; ++f) {
    debug_.foot_target[f] = targets_.foot[f].target;
    debug_.swing_position[f] = targets_.foot[f].swing_position;
    debug_.step_reject[f] = targets_.foot[f].rejected;
  }
  for (u32 j = 0; j < kRigJointCount; ++j) {
    const f32 c = targets_.joint_drive_scale[j] * drive_blend_;  // commanded fraction of budget
    if (c > max_commanded) max_commanded = c;
  }
  debug_.gait_phase = gait_.phase;
  // max_torque_saturation holds the largest fraction of a joint's torque budget
  // commanded this tick (drive_scale * drive_blend), a cheap saturation proxy.
  debug_.max_torque_saturation = max_commanded;
  debug_.mode = mode_;
  debug_.mode_time = mode_time_;

  // Optional env-gated trace (cheap, matches the debug-visibility requirement).
  static const bool kTrace = std::getenv("LOCO_DEBUG") != nullptr;
  if (kTrace && tick_count_ % 10 == 0) {
    std::fprintf(stderr,
                 "[loco] t=%llu mode=%d blend=%.2f pelvis_y=%.3f com=(%.2f,%.2f,%.2f) vz=%.2f "
                 "cp_margin=%.3f tilt=%.2f phase=%.2f sup=%u ph0=%.2f/%d ph1=%.2f/%d "
                 "f0z=%.2f tgt0z=%.2f f1z=%.2f tgt1z=%.2f\n",
                 static_cast<unsigned long long>(tick_count_), static_cast<int>(mode_), drive_blend_,
                 m.root_position.y, m.com_position.x, m.com_position.y, m.com_position.z,
                 m.com_velocity.z, margin, tilt, gait_.phase, contacts_.support_count,
                 GaitClock::FootPhase(gait_, 0), static_cast<int>(contacts_.phase[0]),
                 GaitClock::FootPhase(gait_, 1), static_cast<int>(contacts_.phase[1]),
                 m.foot[0].position.z, targets_.foot[0].target.z, m.foot[1].position.z,
                 targets_.foot[1].target.z);
  }
  ++tick_count_;
}

}  // namespace rx::locomotion
