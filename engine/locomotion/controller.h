#ifndef RX_LOCOMOTION_CONTROLLER_H_
#define RX_LOCOMOTION_CONTROLLER_H_

// LocomotionController: the per-character fixed-update orchestration of
// docs/LOCOMOTION.md. Owns one BipedRig plus the estimator/gait/footstep/
// whole-body subsystems and runs the feedback loop
//
//     intent -> measurements -> contacts -> mode -> gait -> foot plan
//            -> whole-body targets -> joint motor commands
//
// once per fixed physics step, BEFORE physics::PhysicsWorld::Update. It never
// teleports bodies; all actuation goes through joint motors and the bounded
// root assist. No per-tick heap allocation.

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "locomotion/estimator.h"
#include "locomotion/footstep.h"
#include "locomotion/gait.h"
#include "locomotion/rig.h"
#include "locomotion/types.h"

namespace rx::physics {
class PhysicsWorld;
}

namespace rx::locomotion {

class RX_LOCOMOTION_EXPORT LocomotionController {
 public:
  LocomotionController() = default;
  ~LocomotionController();

  LocomotionController(const LocomotionController&) = delete;
  LocomotionController& operator=(const LocomotionController&) = delete;

  // Builds the rig standing at `feet_position` (soles), facing `yaw`. The
  // parameters are copied and immutable afterwards. False when the rig build
  // fails; the controller stays unusable but safe.
  bool Initialize(physics::PhysicsWorld* physics, const ControllerParameters& params,
                  const Vec3& feet_position, f32 yaw);
  void Destroy();

  // One fixed step of control. Call every fixed update with the same dt the
  // physics world will integrate next. Safe (no-op) when uninitialized.
  void Tick(const LocomotionIntent& intent, const PhysicalModifiers& modifiers, f32 dt);

  ControlMode mode() const { return mode_; }
  const CharacterMeasurements& measurements() const { return measurements_; }
  const ContactEstimate& contacts() const { return contacts_; }
  const DebugState& debug() const { return debug_; }
  const BipedRig& rig() const { return rig_; }
  const ControllerParameters& parameters() const { return params_; }
  bool initialized() const { return physics_ != nullptr && rig_.valid(); }

 private:
  // Downward terrain probe wrapping physics_->Raycast, passed to the footstep
  // planner. `context` is the controller (this), so it reaches physics_ without
  // a std::function or captured state.
  static bool GroundProbe(void* context, const Vec3& probe_start, f32 max_depth, GroundHit* out);

  // True when `watched` (pelvis/torso) touches something that is not part of the
  // rig this tick — used by the grounded detection.
  bool IsRigBody(physics::BodyId id) const;
  bool HasEnvironmentContact(physics::BodyId watched) const;

  // World-space ankle pivot of a foot (0 = left, 1 = right), reconstructed from
  // the live foot-body transform. The measured "sole" the estimator reports is
  // the foot-box centre, which sits forward of the ankle by the box offset; the
  // ankle is the true standing pivot and the reference the balance controller
  // keeps the centre of mass over.
  Vec3 AnkleWorld(u32 foot) const;

  // Drives joint `j`'s motor to `torque` (N*m) through the same >1% change gate
  // both the normal and safety-stop actuation paths use, so SetJointDrive (which
  // walks Jolt constraint settings) is re-issued only when the budget moved.
  void ApplyJointDrive(RigJoint j, f32 torque);

  physics::PhysicsWorld* physics_ = nullptr;
  ControllerParameters params_{};
  BipedRig rig_{};

  StateEstimator state_estimator_{};
  ContactEstimator contact_estimator_{};
  GaitClock gait_clock_{};
  FootstepPlanner footstep_planner_{};

  CharacterMeasurements measurements_{};
  ContactEstimate contacts_{};
  GaitState gait_{};
  WholeBodyTargets targets_{};
  DebugState debug_{};

  ControlMode mode_ = ControlMode::kStable;
  f32 mode_time_ = 0;
  f32 drive_blend_ = 1;  // motor-strength blend across mode changes [0..1]
  u64 tick_count_ = 0;   // fixed steps since Initialize (env-gated trace only)

  // Last torque budget actually pushed to each joint motor, so SetJointDrive is
  // re-issued only when the effective drive changes materially (> 1%).
  f32 applied_torque_[kRigJointCount] = {};

  // Mode-machine hysteresis timers (seconds unless noted).
  f32 prev_tilt_ = 0;         // torso tilt last tick (for "tilt rising")
  u32 fall_ticks_ = 0;        // consecutive ticks the fall condition held
  f32 tilt_over_time_ = 0;    // time tilt has stayed past fall_pitch_limit
  f32 no_support_time_ = 0;   // time with zero supporting feet
  f32 grounded_low_time_ = 0; // time grounded conditions have held in kControlledFall
};

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_CONTROLLER_H_
