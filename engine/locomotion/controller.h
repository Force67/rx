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
  physics::PhysicsWorld* physics_ = nullptr;
  ControllerParameters params_{};
  BipedRig rig_{};

  CharacterMeasurements measurements_{};
  ContactEstimate contacts_{};
  GaitState gait_{};
  WholeBodyTargets targets_{};
  DebugState debug_{};

  ControlMode mode_ = ControlMode::kStable;
  f32 mode_time_ = 0;
  f32 drive_blend_ = 1;  // motor-strength blend across mode changes [0..1]
};

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_CONTROLLER_H_
