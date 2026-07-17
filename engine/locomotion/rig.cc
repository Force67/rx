// BipedRig: builds and drives the 13-body / 12-joint articulated ragdoll the
// locomotion controller closes its feedback loop around (docs/LOCOMOTION.md).
//
// Everything is derived from ControllerParameters; there is no authored
// content. Conventions (fixed): right-handed, +Y up, the character faces -Z at
// yaw 0 so character-right is +X and character-LEFT is -X. Metres, kg, radians.
// Spawn yaw follows rx::character::HeadingQuat (rotation about -Y). Every body
// spawns AXIS-ALIGNED (local axes == character axes rotated by the spawn yaw),
// so a joint's twist/plane axes are identical constants in the parent and child
// local frames.
//
// Geometry proportions (all off ControllerParameters L=leg_length,
// W=hip_width, H=body_height, M=total_mass), chosen so the three leg segments
// stack exactly to the hip:
//   foot_height 0.06L + lower_leg 0.44L + upper_leg 0.50L = 1.00L = hip height.
// Radii scale with H/1.75 so a taller/shorter character stays proportionate.

#include "locomotion/rig.h"

#include <cmath>

#include "core/math.h"

namespace rx::locomotion {
namespace {

// A joint's fixed twist (constraint x) and plane/normal (constraint y) axes,
// expressed in the shared axis-aligned character frame. Same in the parent and
// child local space because every body spawns axis-aligned.
struct JointAxes {
  Vec3 twist;
  Vec3 plane;
};

JointAxes AxesFor(RigJoint j) {
  switch (j) {
    case RigJoint::kWaist:
      return {{0, 1, 0}, {1, 0, 0}};  // twist +Y, plane +X
    case RigJoint::kNeck:
      return {{0, 1, 0}, {1, 0, 0}};
    case RigJoint::kHipL:
    case RigJoint::kHipR:
      return {{0, -1, 0}, {1, 0, 0}};  // twist down the limb, plane +X
    case RigJoint::kKneeL:
    case RigJoint::kKneeR:
      return {{-1, 0, 0}, {0, 1, 0}};  // hinge axis -X (character left), normal +Y
    case RigJoint::kAnkleL:
    case RigJoint::kAnkleR:
      return {{0, -1, 0}, {1, 0, 0}};
    case RigJoint::kShoulderL:
    case RigJoint::kShoulderR:
      return {{0, -1, 0}, {1, 0, 0}};  // arm hangs down
    case RigJoint::kElbowL:
    case RigJoint::kElbowR:
      return {{1, 0, 0}, {0, 1, 0}};  // hinge axis +X, normal +Y
    case RigJoint::kCount:
      break;
  }
  return {{1, 0, 0}, {0, 1, 0}};
}

// Quaternion of the child-local constraint frame, matching EXACTLY how Jolt's
// SwingTwistConstraint builds mConstraintToBody from the twist and plane axes
// (SwingTwistConstraint.cpp): constraint X = twist, constraint Y = plane x
// twist, constraint Z = plane. (Note it is NOT (twist, plane, twist x plane) -
// the plane axis is the constraint Z, not Y.) Depends only on the joint's fixed
// axes, so SetJointTarget rebuilds it without storing per-rig state.
Quat ConstraintFrameBasis(RigJoint j) {
  JointAxes a = AxesFor(j);
  Vec3 y = Cross(a.plane, a.twist);  // plane x twist == Jolt's normal axis
  Mat4 m = Mat4::Identity();
  m.m[0] = a.twist.x; m.m[1] = a.twist.y; m.m[2] = a.twist.z;   // col0 = twist
  m.m[4] = y.x;       m.m[5] = y.y;       m.m[6] = y.z;         // col1 = plane x twist
  m.m[8] = a.plane.x; m.m[9] = a.plane.y; m.m[10] = a.plane.z;  // col2 = plane
  return QuatFromMat4(m);
}

// 3x4 row-major body-local joint frame: column 0 = twist/hinge axis, column 1 =
// plane/normal axis, column 3 = origin (the PhysicsWorld FrameToWorld layout).
void FillFrame(f32 frame[12], const Vec3& twist, const Vec3& plane, const Vec3& origin) {
  Vec3 c2 = Cross(twist, plane);
  frame[0] = twist.x; frame[1] = plane.x; frame[2] = c2.x;  frame[3] = origin.x;
  frame[4] = twist.y; frame[5] = plane.y; frame[6] = c2.y;  frame[7] = origin.y;
  frame[8] = twist.z; frame[9] = plane.z; frame[10] = c2.z; frame[11] = origin.z;
}

}  // namespace

f32 BodyMassFraction(BodyPart part) {
  switch (part) {
    case BodyPart::kPelvis: return 0.142f;
    case BodyPart::kTorso: return 0.355f;
    case BodyPart::kHead: return 0.081f;
    case BodyPart::kUpperLegL:
    case BodyPart::kUpperLegR: return 0.10f;
    case BodyPart::kLowerLegL:
    case BodyPart::kLowerLegR: return 0.0465f;
    case BodyPart::kFootL:
    case BodyPart::kFootR: return 0.0145f;
    case BodyPart::kUpperArmL:
    case BodyPart::kUpperArmR: return 0.028f;
    case BodyPart::kLowerArmL:
    case BodyPart::kLowerArmR: return 0.022f;
    case BodyPart::kCount: break;
  }
  return 0;
}

BodyPart JointParent(RigJoint j) {
  switch (j) {
    case RigJoint::kWaist: return BodyPart::kPelvis;
    case RigJoint::kNeck: return BodyPart::kTorso;
    case RigJoint::kHipL: return BodyPart::kPelvis;
    case RigJoint::kKneeL: return BodyPart::kUpperLegL;
    case RigJoint::kAnkleL: return BodyPart::kLowerLegL;
    case RigJoint::kHipR: return BodyPart::kPelvis;
    case RigJoint::kKneeR: return BodyPart::kUpperLegR;
    case RigJoint::kAnkleR: return BodyPart::kLowerLegR;
    case RigJoint::kShoulderL: return BodyPart::kTorso;
    case RigJoint::kElbowL: return BodyPart::kUpperArmL;
    case RigJoint::kShoulderR: return BodyPart::kTorso;
    case RigJoint::kElbowR: return BodyPart::kUpperArmR;
    case RigJoint::kCount: break;
  }
  return BodyPart::kPelvis;
}

BodyPart JointChild(RigJoint j) {
  switch (j) {
    case RigJoint::kWaist: return BodyPart::kTorso;
    case RigJoint::kNeck: return BodyPart::kHead;
    case RigJoint::kHipL: return BodyPart::kUpperLegL;
    case RigJoint::kKneeL: return BodyPart::kLowerLegL;
    case RigJoint::kAnkleL: return BodyPart::kFootL;
    case RigJoint::kHipR: return BodyPart::kUpperLegR;
    case RigJoint::kKneeR: return BodyPart::kLowerLegR;
    case RigJoint::kAnkleR: return BodyPart::kFootR;
    case RigJoint::kShoulderL: return BodyPart::kUpperArmL;
    case RigJoint::kElbowL: return BodyPart::kLowerArmL;
    case RigJoint::kShoulderR: return BodyPart::kUpperArmR;
    case RigJoint::kElbowR: return BodyPart::kLowerArmR;
    case RigJoint::kCount: break;
  }
  return BodyPart::kTorso;
}

bool BipedRig::Build(physics::PhysicsWorld& physics, const ControllerParameters& params,
                     const Vec3& feet_position, f32 yaw, BipedRig* out) {
  *out = BipedRig{};

  const f32 L = params.leg_length;
  const f32 W = params.hip_width;
  const f32 H = params.body_height;
  const f32 M = params.total_mass;
  const f32 rscale = H / 1.75f;
  const f32 half_w = W * 0.5f;

  // Segment lengths (leg segments sum to L).
  const f32 upper_leg = 0.50f * L;
  const f32 lower_leg = 0.44f * L;
  const f32 foot_h = 0.06f * L;
  const f32 foot_len = 0.15f * H;
  const f32 foot_hw = 0.045f * rscale;
  const f32 upper_arm = 0.17f * H;
  const f32 lower_arm = 0.15f * H;

  // Segment radii, all scaled with height.
  const f32 r_upper_leg = 0.055f * rscale;
  const f32 r_lower_leg = 0.045f * rscale;
  const f32 r_arm = 0.04f * rscale;
  const f32 r_head = 0.09f * rscale;
  const f32 r_torso = 0.13f * rscale;
  const f32 r_pelvis = 0.10f * rscale;

  // Vertical stations, measured up from the sole (y = 0 at feet_position).
  const f32 ankle_y = foot_h;
  const f32 knee_y = foot_h + lower_leg;
  const f32 hip_y = L;  // == foot_h + lower_leg + upper_leg
  const f32 pelvis_y = L + r_pelvis;
  const f32 waist_y = L + 2.0f * r_pelvis;  // pelvis capsule top
  const f32 head_center_y = H - r_head;     // crown at H
  const f32 neck_y = head_center_y - 1.5f * r_head;
  const f32 torso_center_y = (waist_y + neck_y) * 0.5f;
  const f32 torso_half = (neck_y - waist_y) * 0.5f;
  const f32 shoulder_y = neck_y - 0.5f * r_torso;
  const f32 elbow_y = shoulder_y - upper_arm;

  const f32 upper_leg_center_y = knee_y + upper_leg * 0.5f;
  const f32 lower_leg_center_y = ankle_y + lower_leg * 0.5f;
  const f32 foot_center_z = -0.2f * foot_len;  // box centred 0.2*len forward of the ankle
  const f32 shoulder_x = r_torso + r_arm;      // arms fully outboard of the torso
  const f32 upper_arm_center_y = shoulder_y - upper_arm * 0.5f;
  const f32 lower_arm_center_y = elbow_y - lower_arm * 0.5f;

  // Derived geometry the controller reads back.
  out->leg_length = L;
  out->upper_leg_length = upper_leg;
  out->lower_leg_length = lower_leg;
  out->foot_height = foot_h;
  out->foot_length = foot_len;
  out->upper_arm_length = upper_arm;
  out->lower_arm_length = lower_arm;
  out->pelvis_height = pelvis_y;
  out->sole_offset = foot_h * 0.5f;
  out->head_radius = r_head;
  out->hip_local[0] = {-half_w, hip_y - pelvis_y, 0};  // left (-X)
  out->hip_local[1] = {+half_w, hip_y - pelvis_y, 0};  // right (+X)

  // Yaw about -Y, matching rx::character::HeadingQuat(yaw) =
  // QuatFromAxisAngle({0, -1, 0}, yaw) = {0, -sin(yaw/2), 0, cos(yaw/2)}.
  const f32 hy = std::sin(yaw * 0.5f);
  const f32 hcw = std::cos(yaw * 0.5f);
  const Quat yawq{0, -hy, 0, hcw};
  const f32 rot[4] = {0, -hy, 0, hcw};
  auto world = [&](const Vec3& local) { return feet_position + Rotate(yawq, local); };

  // --- bodies ---
  const i32 group = physics.CreateBodyFilterGroup(kBodyPartCount);
  if (group < 0) return false;
  out->filter_group = group;

  auto capsule_y = [](f32 half_len, f32 radius) {
    physics::ShapeDesc d;
    d.kind = physics::ShapeDesc::Kind::kCapsule;
    d.a = {0, -half_len, 0};
    d.b = {0, +half_len, 0};
    d.radius = radius;
    return d;
  };

  struct BodySpec {
    BodyPart part;
    physics::ShapeDesc shape;
    Vec3 local_origin;
    f32 friction;
  };

  physics::ShapeDesc pelvis_shape;
  pelvis_shape.kind = physics::ShapeDesc::Kind::kCapsule;  // lateral (local X) axis
  pelvis_shape.a = {-half_w, 0, 0};
  pelvis_shape.b = {+half_w, 0, 0};
  pelvis_shape.radius = r_pelvis;

  physics::ShapeDesc head_shape;
  head_shape.kind = physics::ShapeDesc::Kind::kSphere;
  head_shape.radius = r_head;

  physics::ShapeDesc foot_shape;
  foot_shape.kind = physics::ShapeDesc::Kind::kBox;
  foot_shape.half_extents = {foot_hw, foot_h * 0.5f, foot_len * 0.5f};

  const BodySpec specs[kBodyPartCount] = {
      {BodyPart::kPelvis, pelvis_shape, {0, pelvis_y, 0}, 0.5f},
      {BodyPart::kTorso, capsule_y(torso_half, r_torso), {0, torso_center_y, 0}, 0.5f},
      {BodyPart::kHead, head_shape, {0, head_center_y, 0}, 0.5f},
      {BodyPart::kUpperLegL, capsule_y(upper_leg * 0.5f, r_upper_leg),
       {-half_w, upper_leg_center_y, 0}, 0.5f},
      {BodyPart::kLowerLegL, capsule_y(lower_leg * 0.5f, r_lower_leg),
       {-half_w, lower_leg_center_y, 0}, 0.5f},
      {BodyPart::kFootL, foot_shape, {-half_w, foot_h * 0.5f, foot_center_z}, 0.9f},
      {BodyPart::kUpperLegR, capsule_y(upper_leg * 0.5f, r_upper_leg),
       {+half_w, upper_leg_center_y, 0}, 0.5f},
      {BodyPart::kLowerLegR, capsule_y(lower_leg * 0.5f, r_lower_leg),
       {+half_w, lower_leg_center_y, 0}, 0.5f},
      {BodyPart::kFootR, foot_shape, {+half_w, foot_h * 0.5f, foot_center_z}, 0.9f},
      {BodyPart::kUpperArmL, capsule_y(upper_arm * 0.5f, r_arm),
       {-shoulder_x, upper_arm_center_y, 0}, 0.5f},
      {BodyPart::kLowerArmL, capsule_y(lower_arm * 0.5f, r_arm),
       {-shoulder_x, lower_arm_center_y, 0}, 0.5f},
      {BodyPart::kUpperArmR, capsule_y(upper_arm * 0.5f, r_arm),
       {+shoulder_x, upper_arm_center_y, 0}, 0.5f},
      {BodyPart::kLowerArmR, capsule_y(lower_arm * 0.5f, r_arm),
       {+shoulder_x, lower_arm_center_y, 0}, 0.5f},
  };

  auto rollback = [&]() {
    // Joints before bodies (a live constraint dangles onto a freed body), then
    // release the filter group once its bodies are gone.
    for (u32 j = 0; j < kRigJointCount; ++j) {
      if (out->joint[j]) physics.RemoveJoint(out->joint[j]);
    }
    for (u32 i = 0; i < kBodyPartCount; ++i) {
      if (out->body[i]) physics.RemoveBody(out->body[i]);
    }
    physics.ReleaseBodyFilterGroup(group);
    *out = BipedRig{};
  };

  out->total_mass = M;
  for (u32 i = 0; i < kBodyPartCount; ++i) {
    const BodySpec& s = specs[i];
    const u32 idx = static_cast<u32>(s.part);
    const f32 mass = BodyMassFraction(s.part) * M;
    out->body_mass[idx] = mass;
    const Vec3 pos = world(s.local_origin);
    out->body[idx] = physics.AddDynamicShape(s.shape, pos, rot, 1.0f, mass, s.friction, 0.0f,
                                             group, idx);
    if (!out->body[idx]) {
      rollback();
      return false;
    }
  }

  // --- collision filtering ---
  // One group; each body's subgroup is its BodyPart index. Disable the jointed
  // pairs (they overlap at the shared pivot) plus a few extra overlap-prone
  // non-adjacent pairs.
  auto disable = [&](BodyPart a, BodyPart b) {
    physics.DisableFilterPair(group, static_cast<u32>(a), static_cast<u32>(b));
  };
  for (u32 j = 0; j < kRigJointCount; ++j) {
    disable(JointParent(static_cast<RigJoint>(j)), JointChild(static_cast<RigJoint>(j)));
  }
  disable(BodyPart::kPelvis, BodyPart::kLowerLegL);
  disable(BodyPart::kPelvis, BodyPart::kLowerLegR);
  disable(BodyPart::kTorso, BodyPart::kLowerArmL);
  disable(BodyPart::kTorso, BodyPart::kLowerArmR);
  disable(BodyPart::kPelvis, BodyPart::kUpperArmL);
  disable(BodyPart::kPelvis, BodyPart::kUpperArmR);
  disable(BodyPart::kUpperLegL, BodyPart::kUpperLegR);

  // --- joints ---
  // Twist limits / cones per the fixed convention table; hinges take a [min,max]
  // angle range. Parent is always body a, child body b. Frame origins are the
  // shared pivot expressed in each body's local space.
  struct JointSpec {
    RigJoint joint;
    bool hinge;
    Vec3 pivot;       // world-local (pre-yaw) pivot
    f32 twist_min, twist_max, cone;  // swing-twist
    f32 angle_min, angle_max;        // hinge
  };
  const JointSpec joints[kRigJointCount] = {
      {RigJoint::kWaist, false, {0, waist_y, 0}, -0.5f, 0.5f, 0.5f, 0, 0},
      {RigJoint::kNeck, false, {0, neck_y, 0}, -0.6f, 0.6f, 0.6f, 0, 0},
      {RigJoint::kHipL, false, {-half_w, hip_y, 0}, -0.4f, 0.4f, 1.0f, 0, 0},
      {RigJoint::kKneeL, true, {-half_w, knee_y, 0}, 0, 0, 0, 0.0f, 2.4f},
      {RigJoint::kAnkleL, false, {-half_w, ankle_y, 0}, -0.2f, 0.2f, 0.6f, 0, 0},
      {RigJoint::kHipR, false, {+half_w, hip_y, 0}, -0.4f, 0.4f, 1.0f, 0, 0},
      {RigJoint::kKneeR, true, {+half_w, knee_y, 0}, 0, 0, 0, 0.0f, 2.4f},
      {RigJoint::kAnkleR, false, {+half_w, ankle_y, 0}, -0.2f, 0.2f, 0.6f, 0, 0},
      {RigJoint::kShoulderL, false, {-shoulder_x, shoulder_y, 0}, -0.5f, 0.5f, 1.4f, 0, 0},
      {RigJoint::kElbowL, true, {-shoulder_x, elbow_y, 0}, 0, 0, 0, 0.0f, 2.6f},
      {RigJoint::kShoulderR, false, {+shoulder_x, shoulder_y, 0}, -0.5f, 0.5f, 1.4f, 0, 0},
      {RigJoint::kElbowR, true, {+shoulder_x, elbow_y, 0}, 0, 0, 0, 0.0f, 2.6f},
  };

  for (u32 j = 0; j < kRigJointCount; ++j) {
    const JointSpec& s = joints[j];
    const JointAxes ax = AxesFor(s.joint);
    // specs[] is populated in BodyPart-enum order, so a part indexes it directly.
    const Vec3 parent_origin = specs[static_cast<u32>(JointParent(s.joint))].local_origin;
    const Vec3 child_origin = specs[static_cast<u32>(JointChild(s.joint))].local_origin;
    f32 frame_a[12];
    f32 frame_b[12];
    FillFrame(frame_a, ax.twist, ax.plane, s.pivot - parent_origin);
    FillFrame(frame_b, ax.twist, ax.plane, s.pivot - child_origin);

    const physics::BodyId a = out->body[static_cast<u32>(JointParent(s.joint))];
    const physics::BodyId b = out->body[static_cast<u32>(JointChild(s.joint))];
    physics::JointId handle = 0;
    if (s.hinge) {
      handle = physics.AddHingeJoint(a, b, frame_a, frame_b, 1.0f, s.angle_min, s.angle_max);
    } else {
      handle = physics.AddSwingTwistJoint(a, b, frame_a, frame_b, 1.0f, s.twist_min, s.twist_max,
                                          s.cone, -s.cone, s.cone);
    }
    if (!handle) {
      rollback();
      return false;
    }
    out->joint[j] = handle;

    physics.EnableJointMotors(handle, params.joint_frequency, params.joint_damping);
    physics.SetJointMotorTorqueLimit(handle, params.max_joint_torque);
    f32 bind[4] = {0, 0, 0, 1};
    physics.GetJointOrientation(handle, bind);
    out->bind_constraint[j] = {bind[0], bind[1], bind[2], bind[3]};
  }

  // The controller needs foot contacts now, and torso/pelvis contacts for the
  // grounded detection that follows.
  physics.WatchBodyContacts(out->body[static_cast<u32>(BodyPart::kFootL)]);
  physics.WatchBodyContacts(out->body[static_cast<u32>(BodyPart::kFootR)]);
  physics.WatchBodyContacts(out->body[static_cast<u32>(BodyPart::kPelvis)]);
  physics.WatchBodyContacts(out->body[static_cast<u32>(BodyPart::kTorso)]);
  return true;
}

void BipedRig::Destroy(physics::PhysicsWorld& physics) {
  // Drop the constraints BEFORE their bodies: a joint still registered in the
  // PhysicsSystem holds raw pointers to its two bodies, so the next Update would
  // dereference freed memory if the bodies went first.
  for (u32 j = 0; j < kRigJointCount; ++j) {
    if (joint[j]) physics.RemoveJoint(joint[j]);
  }
  for (u32 i = 0; i < kBodyPartCount; ++i) {
    if (body[i]) physics.RemoveBody(body[i]);
  }
  // Release the shared filter group once its bodies are gone.
  physics.ReleaseBodyFilterGroup(filter_group);
  *this = BipedRig{};
}

void BipedRig::SetJointTarget(physics::PhysicsWorld& physics, RigJoint j,
                              const Quat& local_from_bind) const {
  // `local_from_bind` (L) is the desired child-relative-to-parent delta from
  // bind, in the shared axis-aligned bind frame. Jolt's SetTargetOrientationCS
  // drives GetRotationInConstraintSpace() to the passed quaternion, and (both
  // bodies axis-aligned, sharing one constraint frame cfB) that relative
  // rotation in constraint space is
  //   target_cs = conj(cfB) * L * cfB
  // (SwingTwistConstraint.cpp: q_cs = ConstraintToBody1^-1 * q_bs *
  // ConstraintToBody2, with cfB laid out exactly as Jolt lays out
  // mConstraintToBody). This is the correct, non-mirrored order once cfB uses
  // Jolt's (twist, plane x twist, plane) column layout: the airborne knee sign
  // test passes as specified (L about -X by +0.8 -> sole back + up) and the hip
  // swings cleanly fore-aft with standard right-handed sign.
  const Quat cfB = ConstraintFrameBasis(j);
  const Quat target_cs = Conjugate(cfB) * local_from_bind * cfB;
  const f32 q[4] = {target_cs.x, target_cs.y, target_cs.z, target_cs.w};
  physics.SetJointMotorTarget(joint[static_cast<u32>(j)], q);
}

void BipedRig::SetJointDrive(physics::PhysicsWorld& physics, RigJoint j, f32 frequency, f32 damping,
                             f32 max_torque) const {
  const physics::JointId handle = joint[static_cast<u32>(j)];
  physics.EnableJointMotors(handle, frequency, damping);
  physics.SetJointMotorTorqueLimit(handle, max_torque);
}

Vec3 BipedRig::SolePosition(const physics::PhysicsWorld& physics, u32 foot) const {
  const u32 idx = static_cast<u32>(foot == 0 ? BodyPart::kFootL : BodyPart::kFootR);
  Vec3 pos;
  f32 rot[4] = {0, 0, 0, 1};
  if (!physics.GetBodyTransform(body[idx], &pos, rot)) return {};
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  return pos + Rotate(q, {0, -sole_offset, 0});
}

}  // namespace rx::locomotion
