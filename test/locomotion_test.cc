// Jolt acceptance tests for rx::locomotion's rig + estimators
// (docs/LOCOMOTION.md): rig build sanity, a stiff statue that holds a stand,
// hip/knee motor-direction sign checks, unpowered collapse, and pure
// ContactEstimator hysteresis. Style follows test/character_test.cc: hand-rolled
// Check/Near and a failure counter, plain main returning the count.

#include <cmath>
#include <cstdio>

#include "core/math.h"
#include "locomotion/estimator.h"
#include "locomotion/rig.h"
#include "locomotion/types.h"
#include "physics/physics_world.h"

using namespace rx;
using namespace rx::locomotion;
namespace physics = rx::physics;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "locomotion_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-2f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "locomotion_test: FAIL: %s (got %.4f, expected %.4f)\n", message, actual,
               expected);
  ++failures;
}

// A rig standing on a large floor whose top is at y = 0.
struct Scene {
  physics::PhysicsWorld physics;

  bool Init() {
    if (!physics.Initialize()) return false;
    physics.AddStaticBox({0, -0.5f, 0}, {60, 0.5f, 60});  // top at y = 0
    physics.Update(kDt);
    return true;
  }
};

// Stiffer than the defaults: a statue with box feet is statically stable but
// needs strong motors to hold the bind pose against gravity.
ControllerParameters StiffParams() {
  ControllerParameters p;
  p.joint_frequency = 12;
  p.joint_damping = 1.0f;
  p.max_joint_torque = 900;
  return p;
}

void HoldBind(const BipedRig& rig, physics::PhysicsWorld& phys) {
  for (u32 j = 0; j < kRigJointCount; ++j) {
    rig.SetJointTarget(phys, static_cast<RigJoint>(j), Quat{});
  }
}

// A feedback controller drives its bodies every fixed step, so the ragdoll
// island never sleeps. These pure rig tests have no controller, so keep the
// bodies awake with a zero force (ApplyForce activates unconditionally) - Jolt
// otherwise sleeps the settled statue and freezes motors and contact events.
void KeepAwake(const BipedRig& rig, physics::PhysicsWorld& phys) {
  for (u32 i = 0; i < kBodyPartCount; ++i) phys.ApplyForce(rig.body[i], Vec3{});
}

f32 BodyY(const physics::PhysicsWorld& phys, physics::BodyId id) {
  Vec3 p;
  f32 r[4] = {0, 0, 0, 1};
  phys.GetBodyTransform(id, &p, r);
  return p.y;
}

Vec3 BodyPos(const physics::PhysicsWorld& phys, physics::BodyId id) {
  Vec3 p;
  f32 r[4] = {0, 0, 0, 1};
  phys.GetBodyTransform(id, &p, r);
  return p;
}

bool FiniteVec(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool MeasurementsFinite(const CharacterMeasurements& m) {
  bool ok = FiniteVec(m.root_position) && std::isfinite(m.root_rotation.x) &&
            std::isfinite(m.root_rotation.w) && FiniteVec(m.root_linear_velocity) &&
            FiniteVec(m.root_angular_velocity) && FiniteVec(m.com_position) &&
            FiniteVec(m.com_velocity) && FiniteVec(m.ground_normal) &&
            std::isfinite(m.estimated_body_height);
  for (u32 f = 0; f < kFootCount; ++f) {
    ok = ok && FiniteVec(m.foot[f].position) && FiniteVec(m.foot[f].velocity) &&
         FiniteVec(m.foot[f].contact_normal) && std::isfinite(m.foot[f].contact_impulse) &&
         std::isfinite(m.foot[f].slip_speed);
  }
  return ok;
}

// Builds a rig and settles it into a stand, holding the bind pose.
bool BuildSettled(Scene& s, BipedRig* rig, const ControllerParameters& params, int settle = 90) {
  if (!BipedRig::Build(s.physics, params, {0, 0.02f, 0}, 0.0f, rig)) return false;
  for (int i = 0; i < settle; ++i) {
    KeepAwake(*rig, s.physics);
    HoldBind(*rig, s.physics);
    s.physics.Update(kDt);
  }
  return true;
}

void TestBuild() {
  Scene s;
  if (!s.Init()) {
    Check(false, "physics init (Jolt missing?)");
    return;
  }
  ControllerParameters params;
  BipedRig rig;
  Check(BipedRig::Build(s.physics, params, {0, 0.02f, 0}, 0.0f, &rig), "rig builds");
  s.physics.Update(kDt);

  bool all_bodies = true;
  for (u32 i = 0; i < kBodyPartCount; ++i) all_bodies = all_bodies && rig.body[i] != 0;
  Check(all_bodies, "all 13 body handles are non-zero");

  bool all_joints = true;
  for (u32 j = 0; j < kRigJointCount; ++j) all_joints = all_joints && rig.joint[j] != 0;
  Check(all_joints, "all 12 joint handles are non-zero");

  f32 mass_sum = 0;
  for (u32 i = 0; i < kBodyPartCount; ++i) mass_sum += s.physics.GetBodyMass(rig.body[i]);
  Near(mass_sum, params.total_mass, "body masses sum to total_mass", 0.02f * params.total_mass);

  const Vec3 sole_l = rig.SolePosition(s.physics, 0);
  const Vec3 sole_r = rig.SolePosition(s.physics, 1);
  Check(sole_l.y > -0.01f && sole_l.y < 0.05f, "left sole near y in [0, 0.05]");
  Check(sole_r.y > -0.01f && sole_r.y < 0.05f, "right sole near y in [0, 0.05]");

  bool binds_ok = true;
  for (u32 j = 0; j < kRigJointCount; ++j) binds_ok = binds_ok && std::abs(rig.bind_constraint[j].w) > 0.99f;
  Check(binds_ok, "every bind constraint orientation is near identity (|w| > 0.99)");

  Check(rig.leg_length > 0 && rig.upper_leg_length > 0 && rig.lower_leg_length > 0 &&
            rig.foot_height > 0 && rig.foot_length > 0 && rig.upper_arm_length > 0 &&
            rig.lower_arm_length > 0 && rig.pelvis_height > 0 && rig.sole_offset > 0,
        "all derived rig geometry fields are positive");

  Near(rig.hip_local[0].x, -rig.hip_local[1].x, "hip pivots mirror in x");
  Near(rig.hip_local[0].y, rig.hip_local[1].y, "hip pivots share y");
  Near(rig.hip_local[0].z, rig.hip_local[1].z, "hip pivots share z");
  Check(rig.hip_local[0].x < 0, "left hip pivot is on -X (character left)");

  rig.Destroy(s.physics);
  Check(rig.body[0] == 0, "Destroy zeroes the handles");
}

void TestStandingHold() {
  Scene s;
  if (!s.Init()) return;
  ControllerParameters params = StiffParams();
  BipedRig rig;
  if (!BipedRig::Build(s.physics, params, {0, 0.02f, 0}, 0.0f, &rig)) {
    Check(false, "rig builds for standing hold");
    return;
  }
  const physics::BodyId pelvis = rig.body[static_cast<u32>(BodyPart::kPelvis)];
  const f32 initial_pelvis_y = BodyY(s.physics, pelvis);

  StateEstimator estimator;
  ContactEstimator contacts;
  CharacterMeasurements m;
  for (int i = 0; i < 60; ++i) {
    KeepAwake(rig, s.physics);
    HoldBind(rig, s.physics);
    s.physics.Update(kDt);
    estimator.Measure(s.physics, rig, PhysicalModifiers{}, &m);
    contacts.Update(m, kDt);
  }

  Check(BodyY(s.physics, pelvis) > 0.8f * initial_pelvis_y,
        "pelvis stays above 0.8x its initial height (statue does not topple)");
  Check(m.valid, "measurements are valid after the stand");
  Check(MeasurementsFinite(m), "all measured floats are finite");

  // COM planar position within the feet span (plus a small margin).
  const Vec3 fl = m.foot[0].position;
  const Vec3 fr = m.foot[1].position;
  const f32 min_x = std::min(fl.x, fr.x) - 0.15f;
  const f32 max_x = std::max(fl.x, fr.x) + 0.15f;
  const f32 min_z = std::min(fl.z, fr.z) - 0.25f;
  const f32 max_z = std::max(fl.z, fr.z) + 0.25f;
  Check(m.com_position.x > min_x && m.com_position.x < max_x, "COM x within the feet span");
  Check(m.com_position.z > min_z && m.com_position.z < max_z, "COM z within the feet span");

  Check(contacts.estimate().phase[0] == FootPhase::kSupporting, "left foot reaches kSupporting");
  Check(contacts.estimate().phase[1] == FootPhase::kSupporting, "right foot reaches kSupporting");

  rig.Destroy(s.physics);
}

// Builds the rig hanging in the air with the pelvis pinned kinematic at yaw 0
// (world axes == character axes), so the only thing that moves when a joint is
// driven is that joint's limb - no balance or ground contact to confound the
// direction check.
bool BuildAirborne(Scene& s, BipedRig* rig, int settle = 40) {
  if (!BipedRig::Build(s.physics, StiffParams(), {0, 1.5f, 0}, 0.0f, rig)) return false;
  s.physics.SetBodyKinematic(rig->body[static_cast<u32>(BodyPart::kPelvis)]);
  for (int i = 0; i < settle; ++i) {
    KeepAwake(*rig, s.physics);
    HoldBind(*rig, s.physics);
    s.physics.Update(kDt);
  }
  return true;
}

void TestHipSign() {
  Scene s;
  if (!s.Init()) return;
  BipedRig rig;
  if (!BuildAirborne(s, &rig)) {
    Check(false, "rig builds for hip sign");
    return;
  }
  const physics::BodyId lower_l = rig.body[static_cast<u32>(BodyPart::kLowerLegL)];
  const physics::BodyId lower_r = rig.body[static_cast<u32>(BodyPart::kLowerLegR)];
  const f32 left_z0 = BodyPos(s.physics, lower_l).z;

  // Hip flexion (thigh swings FORWARD, -Z) is a rotation of the down-pointing
  // thigh about character-right (+X). Standard right-handed: R_x(+0.6)*(0,-1,0)
  // = (0,-cos0.6,-sin0.6) -> -Z forward. (docs/LOCOMOTION spec text sketched
  // this with the angle written as -0.6, but its own arithmetic used +0.6; the
  // +0.6 sign is the physically consistent one and matches the knee test's
  // standard convention under the same constraint-space conversion.)
  const Quat flex = QuatFromAxisAngle({1, 0, 0}, 0.6f);
  rig.SetJointDrive(s.physics, RigJoint::kHipL, 16.0f, 1.0f, 1200.0f);
  for (int i = 0; i < 60; ++i) {
    KeepAwake(rig, s.physics);
    HoldBind(rig, s.physics);
    rig.SetJointTarget(s.physics, RigJoint::kHipL, flex);
    s.physics.Update(kDt);
  }

  const f32 left_z = BodyPos(s.physics, lower_l).z;
  const f32 right_z = BodyPos(s.physics, lower_r).z;
  Check(left_z < right_z - 0.05f, "left hip flexion swings the left knee forward (-Z) of the right");
  Check(left_z < left_z0 - 0.05f, "left knee moves forward (-Z) from its hanging position");

  rig.Destroy(s.physics);
}

void TestKneeSign() {
  Scene s;
  if (!s.Init()) return;
  BipedRig rig;
  if (!BuildAirborne(s, &rig)) {
    Check(false, "rig builds for knee sign");
    return;
  }
  const physics::BodyId pelvis = rig.body[static_cast<u32>(BodyPart::kPelvis)];
  const Vec3 sole0 = rig.SolePosition(s.physics, 0) - BodyPos(s.physics, pelvis);

  // Hinge axis -X, positive flexion 0.8 rad: the left sole should swing backward
  // (+Z) and up relative to the pelvis.
  const Quat flex = QuatFromAxisAngle({-1, 0, 0}, 0.8f);
  rig.SetJointDrive(s.physics, RigJoint::kKneeL, 16.0f, 1.0f, 1200.0f);
  for (int i = 0; i < 60; ++i) {
    KeepAwake(rig, s.physics);
    HoldBind(rig, s.physics);
    rig.SetJointTarget(s.physics, RigJoint::kKneeL, flex);
    s.physics.Update(kDt);
  }

  const Vec3 sole = rig.SolePosition(s.physics, 0) - BodyPos(s.physics, pelvis);
  Check(sole.z > sole0.z + 0.05f, "left knee flexion moves the sole backward (+Z rel pelvis)");
  Check(sole.y > sole0.y + 0.03f, "left knee flexion lifts the sole (up rel pelvis)");

  rig.Destroy(s.physics);
}

void TestUnpoweredCollapse() {
  Scene s;
  if (!s.Init()) return;
  BipedRig rig;
  if (!BuildSettled(s, &rig, StiffParams(), 30)) {
    Check(false, "rig builds for collapse");
    return;
  }
  const f32 nominal = rig.pelvis_height;
  for (u32 j = 0; j < kRigJointCount; ++j) s.physics.DisableJointMotors(rig.joint[j]);
  for (int i = 0; i < 120; ++i) {  // 2 s
    KeepAwake(rig, s.physics);
    s.physics.Update(kDt);
  }

  const f32 pelvis_y = BodyY(s.physics, rig.body[static_cast<u32>(BodyPart::kPelvis)]);
  Check(pelvis_y < 0.6f * nominal, "unpowered rig collapses (pelvis below 60% of nominal)");

  rig.Destroy(s.physics);
}

// --- pure ContactEstimator hysteresis (no physics) ---

CharacterMeasurements ContactM(bool c0, bool c1) {
  CharacterMeasurements m;
  m.valid = true;
  const bool c[kFootCount] = {c0, c1};
  for (u32 f = 0; f < kFootCount; ++f) {
    m.foot[f].position = {f == 0 ? -0.12f : 0.12f, 0.0f, 0.0f};
    m.foot[f].velocity = {0, 0, 0};
    m.foot[f].contact_normal = {0, 1, 0};
    m.foot[f].in_contact = c[f];
    m.foot[f].slip_speed = 0;
  }
  return m;
}

void TestContactHysteresis() {
  // Alternating contact / no-contact must never reach supporting.
  {
    ContactEstimator ce;
    ce.Reset();
    for (int i = 0; i < 10; ++i) ce.Update(ContactM(i % 2 == 0, i % 2 == 0), kDt);
    Check(ce.estimate().phase[0] != FootPhase::kSupporting,
          "flickering contact never latches to kSupporting");
  }
  // Three steady contact ticks latch supporting.
  {
    ContactEstimator ce;
    ce.Reset();
    for (int i = 0; i < 3; ++i) ce.Update(ContactM(true, true), kDt);
    Check(ce.estimate().phase[0] == FootPhase::kSupporting, "steady contact reaches kSupporting (L)");
    Check(ce.estimate().phase[1] == FootPhase::kSupporting, "steady contact reaches kSupporting (R)");
    Check(ce.estimate().support_count == 2, "both feet count as support");

    // A single no-contact blip must not drop support.
    ce.Update(ContactM(false, false), kDt);
    Check(ce.estimate().phase[0] == FootPhase::kSupporting, "one blip does not drop support");
    ce.Update(ContactM(true, true), kDt);  // recover

    // Three consecutive no-contact ticks do drop it.
    for (int i = 0; i < 3; ++i) ce.Update(ContactM(false, false), kDt);
    Check(ce.estimate().phase[0] == FootPhase::kSwinging, "sustained no-contact drops to kSwinging");
  }
}

void TestEstimatorFinite() {
  Scene s;
  if (!s.Init()) return;
  BipedRig rig;
  if (!BuildSettled(s, &rig, StiffParams(), 60)) {
    Check(false, "rig builds for finite sweep");
    return;
  }
  StateEstimator estimator;
  CharacterMeasurements m;
  PhysicalModifiers mods;
  mods.carried_mass = 8.0f;
  mods.carried_mass_local_offset = {0, 0.1f, -0.2f};
  estimator.Measure(s.physics, rig, mods, &m);
  Check(m.valid, "measurement with carried mass is valid");
  Check(MeasurementsFinite(m), "every measured float is finite (with carried mass)");
  rig.Destroy(s.physics);
}

}  // namespace

int main() {
  TestBuild();
  TestStandingHold();
  TestHipSign();
  TestKneeSign();
  TestUnpoweredCollapse();
  TestContactHysteresis();
  TestEstimatorFinite();

  if (failures == 0) {
    std::printf("locomotion_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "locomotion_test: %d checks failed\n", failures);
  return 1;
}
