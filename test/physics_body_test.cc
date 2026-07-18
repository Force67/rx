// Exercises the body/joint adapter surface a physics-first locomotion feedback
// controller reads and drives: body velocity/mass/COM, force/torque injection,
// gravity, joint motor torque limits, and per-foot contact recording.
#include <cmath>
#include <cstdio>

#include "physics/physics_world.h"

using namespace rx;
namespace physics = rx::physics;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kGravity = 9.81f;
int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "physics_body_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-2f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "physics_body_test: FAIL: %s (got %.4f, expected %.4f)\n", message, actual,
               expected);
  ++failures;
}

// `actual` within `frac` (fraction, e.g. 0.1 = 10%) of `expected`.
void NearRel(f32 actual, f32 expected, f32 frac, const char* message) {
  Near(actual, expected, message, frac * std::abs(expected));
}

bool Finite(f32 v) { return std::isfinite(v); }
bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

void CheckFinite(const Vec3& v, const char* message) { Check(Finite(v), message); }

// Density and half-extent shared by the free-body tests; mass = density *
// (2h)^3.
constexpr f32 kDensity = 500.0f;
constexpr f32 kHalf = 0.25f;
constexpr f32 kMass = kDensity * (2 * kHalf) * (2 * kHalf) * (2 * kHalf);  // 62.5 kg

physics::PhysicsWorld* MakeWorld(physics::PhysicsWorld& w, const char* who) {
  if (!w.Initialize()) {
    Check(false, who);
    return nullptr;
  }
  return &w;
}

void TestVelocityMassCom() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "velocity/mass/COM: physics init (Jolt missing?)")) return;

  // A box in free fall (no floor) from y = 10.
  const Vec3 start{0, 10, 0};
  physics::BodyId box = w.AddDynamicBox(start, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(box != 0, "fall body created");

  // Mass is density * volume, independent of the sim.
  NearRel(w.GetBodyMass(box), kMass, 0.01f, "GetBodyMass matches density * volume");

  Vec3 com0{};
  Check(w.GetBodyCenterOfMass(box, &com0), "COM readable at rest");
  CheckFinite(com0, "COM finite at rest");

  const int steps = 30;  // 0.5 s
  for (int i = 0; i < steps; ++i) w.Update(kDt);
  const f32 t = steps * kDt;

  Vec3 linear{}, angular{};
  Check(w.GetBodyVelocity(box, &linear, &angular), "velocity readable while falling");
  CheckFinite(linear, "linear velocity finite");
  CheckFinite(angular, "angular velocity finite");
  Check(linear.y < 0, "a falling body has downward vertical velocity");
  // Semi-implicit gravity integration: v_y = -g * t.
  NearRel(linear.y, -kGravity * t, 0.10f, "vertical velocity tracks -g * t");

  Vec3 com1{};
  Check(w.GetBodyCenterOfMass(box, &com1), "COM readable while falling");
  CheckFinite(com1, "COM finite while falling");
  Check(com1.y < com0.y - 0.5f, "COM tracks the fall downward");

  // A null output pointer is tolerated (linear-only or angular-only reads).
  Vec3 lin_only{};
  Check(w.GetBodyVelocity(box, &lin_only, nullptr), "velocity with null angular output");

  // Dead handles: 0 and a bogus id read back as false / 0.
  Vec3 dummy{};
  Check(!w.GetBodyVelocity(0, &dummy, &dummy), "GetBodyVelocity(0) is false");
  Check(!w.GetBodyVelocity(987654321u, &dummy, &dummy), "GetBodyVelocity(bogus) is false");
  Near(w.GetBodyMass(0), 0.0f, "GetBodyMass(0) is 0");
  Near(w.GetBodyMass(987654321u), 0.0f, "GetBodyMass(bogus) is 0");
  Check(!w.GetBodyCenterOfMass(0, &dummy), "GetBodyCenterOfMass(0) is false");
  Check(!w.GetBodyCenterOfMass(987654321u, &dummy), "GetBodyCenterOfMass(bogus) is false");

  // A static body has no mass.
  physics::BodyId floor = w.AddStaticBox({0, -0.5f, 0}, {5, 0.5f, 5});
  Near(w.GetBodyMass(floor), 0.0f, "static body reports 0 mass");

  physics::BodyId pinned = w.AddDynamicBox({2, 10, 0}, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(w.GetBodyMass(pinned) > 0, "dynamic body has finite mass before pinning");
  w.SetBodyKinematic(pinned);
  Near(w.GetBodyMass(pinned), 0.0f, "kinematic body reports 0 mass");
}

void TestApplyForce() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "ApplyForce: physics init")) return;

  physics::BodyId box = w.AddDynamicBox({0, 10, 0}, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(box != 0, "force body created");

  // Constant +X force re-applied every step (forces are consumed per step).
  const Vec3 force{100, 0, 0};
  const int steps = 60;  // 1 s
  for (int i = 0; i < steps; ++i) {
    w.ApplyForce(box, force);
    w.Update(kDt);
  }
  const f32 t = steps * kDt;

  Vec3 linear{}, angular{};
  Check(w.GetBodyVelocity(box, &linear, &angular), "velocity readable after forcing");
  CheckFinite(linear, "forced velocity finite");
  // v_x = F / m * t; gravity only touches y so it does not perturb this.
  NearRel(linear.x, force.x / kMass * t, 0.15f, "ApplyForce accelerates by F/m");
  Check(linear.y < 0, "gravity still pulls the forced body down on y");
}

void TestApplyTorque() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "ApplyTorque: physics init")) return;

  physics::BodyId box = w.AddDynamicBox({0, 10, 0}, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(box != 0, "torque body created");

  // Spin up about +Y.
  const Vec3 torque{0, 5, 0};
  auto spin = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      w.ApplyTorque(box, torque);
      w.Update(kDt);
    }
  };

  spin(10);
  Vec3 lin{}, ang1{};
  Check(w.GetBodyVelocity(box, &lin, &ang1), "angular velocity readable");
  CheckFinite(ang1, "angular velocity finite (early)");
  Check(ang1.y > 0, "torque about +Y spins the body positively about Y");
  Check(std::abs(ang1.y) > std::abs(ang1.x) && std::abs(ang1.y) > std::abs(ang1.z),
        "spin stays about the applied (Y) axis");

  spin(20);
  Vec3 ang2{};
  Check(w.GetBodyVelocity(box, &lin, &ang2), "angular velocity readable (later)");
  Check(ang2.y > ang1.y + 1e-3f, "continued torque grows the angular velocity");
}

void TestGravity() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "gravity: physics init")) return;
  const Vec3 g = w.gravity();
  CheckFinite(g, "gravity vector finite");
  Near(g.x, 0.0f, "gravity has no x");
  Near(g.z, 0.0f, "gravity has no z");
  Near(g.y, -kGravity, "gravity magnitude on -y", 0.05f);
}

void TestRaycastIgnoresMultipleBodies() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "ray ignore: physics init")) return;

  w.AddStaticBox({0, -0.5f, 0}, {5, 0.5f, 5});
  const physics::BodyId lower = w.AddKinematicBox({0, 1.0f, 0}, {0.5f, 0.5f, 0.5f});
  const physics::BodyId upper = w.AddKinematicBox({0, 3.0f, 0}, {0.5f, 0.5f, 0.5f});
  Check(lower != 0 && upper != 0, "ray-ignore bodies created");

  physics::PhysicsWorld::RayHit hit;
  Check(w.Raycast({0, 5, 0}, {0, -1, 0}, 10, &hit), "unfiltered ray hits upper body");
  Near(hit.position.y, 3.5f, "unfiltered ray returns upper body", 0.05f);

  Check(w.Raycast({0, 5, 0}, {0, -1, 0}, 10, &hit, upper), "single-ignore ray hits lower body");
  Near(hit.position.y, 1.5f, "single-ignore ray returns lower body", 0.05f);

  const physics::BodyId ignored[] = {upper, lower};
  Check(w.Raycast({0, 5, 0}, {0, -1, 0}, 10, &hit, ignored, 2),
        "multi-ignore ray reaches floor");
  Near(hit.position.y, 0.0f, "multi-ignore ray excludes every listed body", 0.05f);
}

void TestContacts() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "contacts: physics init")) return;

  w.AddStaticBox({0, -0.5f, 0}, {10, 0.5f, 10});  // floor top at y = 0
  physics::BodyId box = w.AddDynamicBox({0, 1.0f, 0}, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(box != 0, "contact body created");
  w.WatchBodyContacts(box);

  // Poll every step across the drop and settle: contacts are recorded while the
  // box is in active contact (from touchdown until Jolt lets it sleep), so we
  // sweep the window rather than a single frame that might land on sleep.
  bool saw_contact = false;
  bool floor_like = false;
  for (int i = 0; i < 120; ++i) {
    w.Update(kDt);
    physics::PhysicsWorld::BodyContact contacts[8];
    const u32 n = w.GetBodyContacts(box, contacts, 8);
    if (n >= 1) saw_contact = true;
    for (u32 j = 0; j < n; ++j) {
      const physics::PhysicsWorld::BodyContact& c = contacts[j];
      CheckFinite(c.position, "contact position finite");
      CheckFinite(c.normal, "contact normal finite");
      Check(Finite(c.impulse), "contact impulse finite");
      // Normal points INTO the box, i.e. up for a box resting on the floor;
      // the contact sits at the box's bottom face (y ~ 0).
      if (c.normal.y > 0.7f && std::abs(c.position.y) < 0.1f) floor_like = true;
    }
  }
  Check(saw_contact, "a box landing on the floor reports at least one contact");
  Check(floor_like, "contact normal points up and sits at the box bottom");

  // Unwatching drops the body's record immediately: empty now and after a step.
  physics::PhysicsWorld::BodyContact after[8];
  w.UnwatchBodyContacts(box);
  Check(w.GetBodyContacts(box, after, 8) == 0, "unwatched body reports no contacts");
  w.Update(kDt);
  Check(w.GetBodyContacts(box, after, 8) == 0, "unwatched body stays empty after a step");
}

void TestMotors() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "motors: physics init")) return;

  // Two boxes side by side (A left, B right) joined at the gap; A is pinned
  // kinematic so B hangs off it horizontally. Gravity torques B downward about
  // the pivot, so only a motor holding the bind orientation keeps it up.
  const Vec3 half{kHalf, kHalf, kHalf};
  physics::BodyId a = w.AddDynamicBox({0.0f, 5.0f, 0.0f}, half, kDensity, {});
  physics::BodyId b = w.AddDynamicBox({0.8f, 5.0f, 0.0f}, half, kDensity, {});
  Check(a != 0 && b != 0, "motor bodies created");

  // 3x4 row-major frames: identity basis rows (twist = X, plane = Y), origin
  // column at the pivot (0.4, 5, 0) in each body's local space.
  const f32 frame_a[12] = {1, 0, 0, 0.4f, 0, 1, 0, 0, 0, 0, 1, 0};
  const f32 frame_b[12] = {1, 0, 0, -0.4f, 0, 1, 0, 0, 0, 0, 1, 0};
  // Wide swing/twist limits so the joint itself never resists; the motor does.
  physics::JointId joint =
      w.AddSwingTwistJoint(a, b, frame_a, frame_b, 1.0f, -3.14f, 3.14f, 3.0f, -3.0f, 3.0f);
  Check(joint != 0, "swing-twist joint created");

  w.EnableJointMotors(joint, 20.0f, 1.0f);
  f32 target[4];
  Check(w.GetJointOrientation(joint, target), "snapshot bind orientation");
  Check(Finite(target[0]) && Finite(target[1]) && Finite(target[2]) && Finite(target[3]),
        "bind orientation finite");
  w.SetJointMotorTarget(joint, target);
  w.SetBodyKinematic(a);  // hang B off a pinned A

  auto step = [&](int count) {
    for (int i = 0; i < count; ++i) w.Update(kDt);
  };
  // A body the motor is holding perfectly still goes to sleep within ~0.5 s;
  // nudge B awake (zero force still reactivates it) so a subsequent motor
  // change actually takes effect instead of being ignored on a sleeping
  // ragdoll.
  auto wake = [&]() { w.ApplyForce(b, {0, 0, 0}); };
  auto dot_target = [&]() -> f32 {
    f32 q[4] = {0, 0, 0, 1};
    Check(w.GetJointOrientation(joint, q), "joint orientation readable");
    Check(Finite(q[0]) && Finite(q[1]) && Finite(q[2]) && Finite(q[3]), "joint orientation finite");
    // |dot| of unit quaternions = cos(half-angle); 1 means aligned.
    return std::abs(q[0] * target[0] + q[1] * target[1] + q[2] * target[2] + q[3] * target[3]);
  };

  step(120);  // 2 s
  Check(dot_target() > 0.99f, "an unlimited motor holds B near the bind pose under gravity");

  // Choke the motor to almost nothing: gravity now pulls B off the target.
  w.SetJointMotorTorqueLimit(joint, 0.01f);
  wake();
  step(120);
  Check(dot_target() < 0.99f, "a tiny torque limit lets gravity drag B off the target");

  // Restore authority to pull B back up to the bind pose, confirming the limit
  // (not the motor state) was the constraint.
  w.SetJointMotorTorqueLimit(joint, 1.0e6f);
  wake();
  step(120);
  Check(dot_target() > 0.99f, "raising the torque limit reasserts the hold");

  // Turn the motors off entirely: B (at the bind pose) falls free and its
  // orientation keeps changing frame to frame.
  w.DisableJointMotors(joint);
  wake();
  f32 before[4] = {0, 0, 0, 1};
  w.GetJointOrientation(joint, before);
  step(20);
  f32 after[4] = {0, 0, 0, 1};
  w.GetJointOrientation(joint, after);
  const f32 moved = std::abs(before[0] * after[0] + before[1] * after[1] + before[2] * after[2] +
                             before[3] * after[3]);
  Check(moved < 0.999f, "disabled motors let the joint swing free");
}

// Tearing a joint down before its bodies must not leave a dangling constraint
// that the next Update dereferences. Removing the joint clears the entry so the
// joint APIs no-op on the stale handle.
void TestRemoveJoint() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "RemoveJoint: physics init")) return;

  const Vec3 half{kHalf, kHalf, kHalf};
  physics::BodyId a = w.AddDynamicBox({0.0f, 5.0f, 0.0f}, half, kDensity, {});
  physics::BodyId b = w.AddDynamicBox({0.8f, 5.0f, 0.0f}, half, kDensity, {});
  Check(a != 0 && b != 0, "RemoveJoint bodies created");

  const f32 frame_a[12] = {1, 0, 0, 0.4f, 0, 1, 0, 0, 0, 0, 1, 0};
  const f32 frame_b[12] = {1, 0, 0, -0.4f, 0, 1, 0, 0, 0, 0, 1, 0};
  physics::JointId joint =
      w.AddSwingTwistJoint(a, b, frame_a, frame_b, 1.0f, -3.14f, 3.14f, 3.0f, -3.0f, 3.0f);
  Check(joint != 0, "RemoveJoint joint created");
  w.EnableJointMotors(joint, 20.0f, 1.0f);

  f32 q[4] = {0, 0, 0, 1};
  Check(w.GetJointOrientation(joint, q), "joint orientation readable before removal");

  // Drop the constraint FIRST, then both bodies — the previously-crashing order
  // (bodies removed while the constraint stayed registered) left the next Update
  // dereferencing freed body pointers.
  w.RemoveJoint(joint);
  Check(!w.GetJointOrientation(joint, q), "GetJointOrientation is false after RemoveJoint");
  // Every joint motor API no-ops on the cleared handle (no crash, no effect).
  w.EnableJointMotors(joint, 20.0f, 1.0f);
  w.SetJointMotorTarget(joint, q);
  w.SetJointMotorTorqueLimit(joint, 1.0f);
  w.DisableJointMotors(joint);
  w.RemoveJoint(joint);  // idempotent

  w.RemoveBody(a);
  w.RemoveBody(b);

  // Stepping after the joint + bodies are gone must not crash (exercises the
  // constraint-vs-freed-body path that used to fault).
  for (int i = 0; i < 10; ++i) w.Update(kDt);
  Check(true, "world steps cleanly after RemoveJoint + RemoveBody");
}

// RemoveBody must drop a body's watched-contact entry so the recorder's list
// does not leak, and a fresh body watched afterward still records contacts.
void TestWatchedContactReuse() {
  physics::PhysicsWorld w;
  if (!MakeWorld(w, "watched reuse: physics init")) return;

  w.AddStaticBox({0, -0.5f, 0}, {10, 0.5f, 10});  // floor top at y = 0

  physics::BodyId first = w.AddDynamicBox({0, 1.0f, 0}, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(first != 0, "first watched body created");
  w.WatchBodyContacts(first);
  w.RemoveBody(first);  // drops the watched entry (else it leaks / can alias)

  physics::BodyId second = w.AddDynamicBox({0, 1.0f, 0}, {kHalf, kHalf, kHalf}, kDensity, {});
  Check(second != 0, "second watched body created");
  physics::PhysicsWorld::BodyContact stale[8];
  Check(w.GetBodyContacts(first, stale, 8) == 0, "removed body reports no contacts");
  w.WatchBodyContacts(second);

  bool saw_contact = false;
  for (int i = 0; i < 120; ++i) {
    w.Update(kDt);
    physics::PhysicsWorld::BodyContact contacts[8];
    if (w.GetBodyContacts(second, contacts, 8) >= 1) saw_contact = true;
  }
  Check(saw_contact, "a new body watched after a removal still records contacts");
}

}  // namespace

int main() {
  TestVelocityMassCom();
  TestApplyForce();
  TestApplyTorque();
  TestGravity();
  TestRaycastIgnoresMultipleBodies();
  TestContacts();
  TestMotors();
  TestRemoveJoint();
  TestWatchedContactReuse();

  if (failures == 0) {
    std::printf("physics_body_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "physics_body_test: %d checks failed\n", failures);
  return 1;
}
