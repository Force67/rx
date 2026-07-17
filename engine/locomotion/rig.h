#ifndef RX_LOCOMOTION_RIG_H_
#define RX_LOCOMOTION_RIG_H_

// BipedRig: the articulated physical body the locomotion controller drives.
// Built entirely from ControllerParameters (no authored content): 13 capsule
// bodies joined by swing-twist and hinge constraints with position motors
// (physics::PhysicsWorld::EnableJointMotors / SetJointMotorTarget).
//
// The rig also owns the one tricky convention conversion: controller code
// thinks in joint-LOCAL child-relative-to-parent rotations relative to the
// bind pose (identity = bind), while Jolt motors want CONSTRAINT-SPACE
// orientations. Build() snapshots each joint's bind constraint orientation and
// SetJointTarget() composes the two, so nothing outside rig.cc touches
// constraint space.

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "locomotion/types.h"
#include "physics/physics_world.h"

namespace rx::locomotion {

struct RX_LOCOMOTION_EXPORT BipedRig {
  physics::BodyId body[kBodyPartCount] = {};
  physics::JointId joint[kRigJointCount] = {};
  f32 body_mass[kBodyPartCount] = {};        // kg, resolved at build
  Quat bind_constraint[kRigJointCount] = {}; // constraint-space bind snapshot
  f32 total_mass = 0;

  // Derived geometry the controller reads back (metres).
  f32 leg_length = 0;      // hip pivot to sole
  f32 foot_length = 0;
  f32 pelvis_height = 0;   // pelvis body origin above the soles at bind
  f32 sole_offset = 0;     // foot body origin to sole along body -Y

  // Builds the ragdoll standing with its soles at `feet_position`, facing yaw
  // radians (yaw 0 faces -Z). All bodies share one collision filter group with
  // adjacent-pair collisions disabled. Motors are enabled on every joint at
  // the parameter drive and hold the bind pose. False (and no bodies) when the
  // physics world rejects any piece.
  static bool Build(physics::PhysicsWorld& physics, const ControllerParameters& params,
                    const Vec3& feet_position, f32 yaw, BipedRig* out);
  void Destroy(physics::PhysicsWorld& physics);

  // Drives `j`'s motor toward `local_from_bind`: the desired rotation of the
  // child body relative to the parent body, expressed relative to the bind
  // pose (identity holds bind). Handles the constraint-space conversion.
  void SetJointTarget(physics::PhysicsWorld& physics, RigJoint j,
                      const Quat& local_from_bind) const;

  // Re-tunes a joint's motor spring and torque budget (blend these, never
  // step them, when changing control regimes).
  void SetJointDrive(physics::PhysicsWorld& physics, RigJoint j, f32 frequency, f32 damping,
                     f32 max_torque) const;

  bool valid() const { return body[0] != 0; }
};

// Fixed anthropometric mass fractions used by Build (sum ~1); exposed for the
// state estimator's COM weighting and for tests.
RX_LOCOMOTION_EXPORT f32 BodyMassFraction(BodyPart part);

// Parent body of each joint's child link, for measurement/IK traversal.
RX_LOCOMOTION_EXPORT BodyPart JointParent(RigJoint j);
RX_LOCOMOTION_EXPORT BodyPart JointChild(RigJoint j);

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_RIG_H_
