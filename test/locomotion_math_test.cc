// Pure-math locomotion tests (docs/LOCOMOTION.md): the gait clock, capture
// point, swing trajectory and footstep planner. No Jolt / physics involved —
// terrain is faked through a synthetic downward probe. Hand-rolled Check/Near
// harness in the rx test style (see test/camera_test.cc).

#include <cmath>
#include <cstdio>

#include "core/math.h"
#include "core/types.h"
#include "locomotion/footstep.h"
#include "locomotion/gait.h"
#include "locomotion/rig.h"
#include "locomotion/types.h"
#include "locomotion/whole_body.h"

namespace {

using namespace rx;
using namespace rx::locomotion;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "locomotion_math_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-4f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "locomotion_math_test: FAIL: %s (got %.6f, expected %.6f)\n", message,
               actual, expected);
  ++failures;
}

bool Finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Finite(const Quat& q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

// --- synthetic terrain probes
// -------------------------------------------------

struct FlatFloor {
  f32 y = 0;
};

bool FlatProbe(void* context, const Vec3& probe_start, f32 max_depth, GroundHit* out) {
  const FlatFloor* floor = static_cast<const FlatFloor*>(context);
  if (floor->y > probe_start.y) return false;              // floor above the probe
  if (probe_start.y - floor->y > max_depth) return false;  // out of reach
  out->position = {probe_start.x, floor->y, probe_start.z};
  out->normal = {0, 1, 0};
  return true;
}

// Always hits, but on a slope steeper than the default max_ground_slope (0.7
// rad): acos(0.6) ~= 0.93 rad.
bool SteepProbe(void*, const Vec3& probe_start, f32, GroundHit* out) {
  out->position = {probe_start.x, 0, probe_start.z};
  out->normal = Normalize(Vec3{0.8f, 0.6f, 0});
  return true;
}

bool NoHitProbe(void*, const Vec3&, f32, GroundHit*) { return false; }

// --- measurement builders
// -----------------------------------------------------

CharacterMeasurements MakeMeasurements(const ControllerParameters& params, const Vec3& com_vel) {
  CharacterMeasurements m;
  m.valid = true;
  m.root_position = {0, 0.95f, 0};
  m.root_rotation = {0, 0, 0, 1};
  m.com_position = {0, 0.95f, 0};
  m.com_velocity = com_vel;
  const f32 half = params.hip_width * 0.5f;
  m.foot[0].position = {-half, 0, 0};
  m.foot[1].position = {half, 0, 0};
  m.foot[0].contact_normal = {0, 1, 0};
  m.foot[1].contact_normal = {0, 1, 0};
  m.foot[0].in_contact = true;
  m.foot[1].in_contact = true;
  return m;
}

ContactEstimate MakeContacts() {
  ContactEstimate c;
  c.phase[0] = FootPhase::kSupporting;
  c.phase[1] = FootPhase::kSupporting;
  c.support_center = {0, 0, 0};
  c.support_count = 2;
  return c;
}

// --- Wrap01
// -------------------------------------------------------------------

void TestWrap01() {
  Near(Wrap01(0.25f), 0.25f, "wrap in range");
  Near(Wrap01(-0.25f), 0.75f, "wrap negative");
  Near(Wrap01(1.25f), 0.25f, "wrap above one");
  Near(Wrap01(1.0f), 0.0f, "wrap exactly one -> zero");
  Near(Wrap01(-1.0f), 0.0f, "wrap negative one -> zero");
  Near(Wrap01(2.5f), 0.5f, "wrap far above");
}

// --- GaitClock
// ----------------------------------------------------------------

void TestGaitFeetOppose() {
  GaitState s;
  s.phase = 0.3f;
  s.stance_fraction = 0.6f;
  const f32 fp0 = GaitClock::FootPhase(s, 0);
  const f32 fp1 = GaitClock::FootPhase(s, 1);
  Near(fp0, 0.3f, "left foot keys phase");
  Near(Wrap01(fp1 - fp0), 0.5f, "feet oppose by half a cycle");
}

void TestGaitCycleAndStance() {
  const ControllerParameters params;
  const f32 dt = 1.0f / 120.0f;
  GaitClock clock;
  LocomotionIntent intent;
  intent.desired_velocity = {0, 0, -params.walk_speed};
  const CharacterMeasurements m = MakeMeasurements(params, {0, 0, -params.walk_speed});

  for (int i = 0; i < 600; ++i) clock.Update(m, intent, params, false, dt);  // settle
  const f32 rate = clock.state().phase_rate;
  Check(rate > params.walk_stride_frequency - 1e-3f && rate < params.run_stride_frequency + 1e-3f,
        "settled phase rate within the gait envelope");

  // A full cycle takes ~1/phase_rate seconds (the settled stride frequency).
  f32 total = 0;
  f32 prev = clock.state().phase;
  int ticks = 0;
  while (total < 1.0f && ticks < 100000) {
    clock.Update(m, intent, params, false, dt);
    f32 cur = clock.state().phase;
    f32 d = cur - prev;
    if (d < 0) d += 1;  // unwrap
    total += d;
    prev = cur;
    ++ticks;
  }
  Near(ticks * dt, 1.0f / rate, "full cycle takes ~1/phase_rate", 0.05f);

  // Stance share of the cycle matches stance_fraction (foot 0 keys phase).
  GaitState s;
  s.stance_fraction = clock.state().stance_fraction;
  const int samples = 10000;
  int in_stance = 0;
  for (int i = 0; i < samples; ++i) {
    s.phase = static_cast<f32>(i) / samples;
    if (GaitClock::InStance(s, 0)) ++in_stance;
  }
  Near(static_cast<f32>(in_stance) / samples, s.stance_fraction, "stance fraction consistent",
       0.01f);

  // Swing progress spans 0..1 across the swing interval.
  s.phase = s.stance_fraction;
  Near(GaitClock::SwingProgress(s, 0), 0.0f, "swing progress 0 at lift-off");
  Near(GaitClock::InStance(s, 0) ? 0.0f : 1.0f, 1.0f,
       "at stance_fraction the foot has left stance");
}

void TestGaitStartStop() {
  const ControllerParameters params;
  const f32 dt = 1.0f / 120.0f;

  // Stationary intent from rest: never steps, phase frozen.
  GaitClock idle;
  LocomotionIntent still;
  still.desired_velocity = {0, 0, 0};
  const CharacterMeasurements ms = MakeMeasurements(params, {0, 0, 0});
  for (int i = 0; i < 100; ++i) idle.Update(ms, still, params, false, dt);
  Check(!idle.state().stepping, "stationary intent does not step");
  const f32 frozen = idle.state().phase;
  for (int i = 0; i < 100; ++i) idle.Update(ms, still, params, false, dt);
  Near(idle.state().phase, frozen, "phase frozen while parked");

  // need_step forces stepping even at zero desired speed.
  GaitClock forced;
  for (int i = 0; i < 20; ++i) forced.Update(ms, still, params, true, dt);
  Check(forced.state().stepping, "need_step forces stepping");
  Check(forced.state().phase > 0.0f, "forced stepping advances the phase");

  // Walking, then a stop command: keeps stepping until a double-support point.
  GaitClock walker;
  LocomotionIntent walk;
  walk.desired_velocity = {0, 0, -params.walk_speed};
  const CharacterMeasurements mw = MakeMeasurements(params, {0, 0, -params.walk_speed});
  for (int i = 0; i < 400; ++i) walker.Update(mw, walk, params, false, dt);
  Check(walker.state().stepping, "walking intent steps");

  const CharacterMeasurements mslow = MakeMeasurements(params, {0, 0, 0});
  int guard = 0;
  while (walker.state().stepping && guard < 100000) {
    walker.Update(mslow, still, params, false, dt);
    ++guard;
  }
  Check(!walker.state().stepping, "stops once slowed");
  Check(GaitClock::InStance(walker.state(), 0) && GaitClock::InStance(walker.state(), 1),
        "stops parked in double support");
}

void TestGaitRampSmooth() {
  const ControllerParameters params;
  const f32 dt = 1.0f / 120.0f;
  GaitClock clock;
  LocomotionIntent run;
  run.desired_velocity = {0, 0, -params.run_speed};
  const CharacterMeasurements m = MakeMeasurements(params, {0, 0, -params.run_speed});

  f32 prev_rate = -1;
  bool smooth = true;
  for (int i = 0; i < 400; ++i) {
    clock.Update(m, run, params, false, dt);
    const f32 rate = clock.state().phase_rate;
    if (prev_rate > 0 && std::abs(rate - prev_rate) / prev_rate > 0.2f) smooth = false;
    prev_rate = rate;
  }
  Check(smooth, "phase rate ramps smoothly (<20% per tick)");
  Near(clock.state().speed_ratio, 1.0f, "settles at run speed ratio", 0.02f);
}

// --- CapturePoint
// -------------------------------------------------------------

void TestCapturePoint() {
  const Vec3 com{1, 0.95f, 2};
  const Vec3 cp0 = CapturePoint(com, {0, 0, 0}, 9.81f, 0.95f);
  Near(cp0.x, com.x, "zero-velocity capture == COM x");
  Near(cp0.y, com.y, "zero-velocity capture == COM y");
  Near(cp0.z, com.z, "zero-velocity capture == COM z");

  const Vec3 v{0.5f, 0, -0.3f};
  const Vec3 cp1 = CapturePoint(com, v, 9.81f, 0.95f);
  const f32 omega = std::sqrt(9.81f / 0.95f);
  Near(cp1.x - com.x, v.x / omega, "capture offset x = v/sqrt(g/h)");
  Near(cp1.z - com.z, v.z / omega, "capture offset z = v/sqrt(g/h)");

  const Vec3 cp2 = CapturePoint(com, v, 9.81f, 0.0f);
  Check(Finite(cp2), "zero COM height does not NaN");
}

// --- Swing trajectory
// ---------------------------------------------------------

void TestSwingTrajectory() {
  const Vec3 start{0, 0, 0};
  const Vec3 target{1, 0, 2};
  const f32 h = 0.12f;

  const Vec3 p0 = SwingPosition(start, target, h, 0);
  Near(p0.x, start.x, "swing t=0 -> start x");
  Near(p0.y, start.y, "swing t=0 -> start y");
  Near(p0.z, start.z, "swing t=0 -> start z");

  const Vec3 p1 = SwingPosition(start, target, h, 1);
  Near(p1.x, target.x, "swing t=1 -> target x");
  Near(p1.y, target.y, "swing t=1 -> target y");
  Near(p1.z, target.z, "swing t=1 -> target z");

  const Vec3 pm = SwingPosition(start, target, h, 0.5f);
  const Vec3 mid = (start + target) * 0.5f;
  Near(pm.x, mid.x, "swing apex x = midpoint");
  Near(pm.z, mid.z, "swing apex z = midpoint");
  Near(pm.y, mid.y + h, "swing apex y = chord + step_height");

  f32 prev_x = -1e9f;
  bool mono = true;
  for (int i = 0; i <= 20; ++i) {
    const f32 t = static_cast<f32>(i) / 20.0f;
    const Vec3 p = SwingPosition(start, target, h, t);
    if (p.x < prev_x - 1e-5f) mono = false;
    prev_x = p.x;
  }
  Check(mono, "swing horizontal progress is monotonic");

  // SwingVelocity ~= finite difference of SwingPosition (t_rate = 1).
  const f32 dt = 1e-3f;
  const f32 ts[] = {0.25f, 0.5f, 0.75f};
  for (f32 t : ts) {
    const Vec3 va = SwingVelocity(start, target, h, t, 1.0f);
    const Vec3 fd =
        (SwingPosition(start, target, h, t + dt) - SwingPosition(start, target, h, t - dt)) *
        (1.0f / (2.0f * dt));
    const f32 tol = 0.05f * (Length(fd) + 0.05f);
    Near(va.x, fd.x, "swing velocity x matches finite difference", tol);
    Near(va.y, fd.y, "swing velocity y matches finite difference", tol);
    Near(va.z, fd.z, "swing velocity z matches finite difference", tol);
  }
}

// --- FootstepPlanner
// ----------------------------------------------------------

void TestFootstepPlanner() {
  const ControllerParameters params;
  const CharacterMeasurements m = MakeMeasurements(params, {0, 0, -1.0f});
  const ContactEstimate contacts = MakeContacts();
  LocomotionIntent intent;
  intent.desired_velocity = {0, 0, -1.0f};
  intent.desired_facing = {0, 0, -1};
  FlatFloor floor{0.0f};
  const f32 dt = 1.0f / 120.0f;

  // Phase 0.75 with a 0.5 stance fraction -> left foot swings, right plants.
  GaitState g;
  g.stepping = true;
  g.stance_fraction = 0.5f;
  g.phase = 0.75f;
  g.phase_rate = 1.8f;
  g.speed_ratio = 0.5f;

  FootstepPlanner planner;
  planner.Update(m, contacts, g, intent, params, FlatProbe, &floor, dt);
  const FootPlan& left = planner.plan(0);
  const FootPlan& right = planner.plan(1);

  Check(left.swinging, "left foot swings at phase 0.75");
  Check(!right.swinging, "right foot plants at phase 0.75");
  Check(left.rejected == StepReject::kNone, "flat floor accepts the step");
  Check(left.target.z < 0, "left step lands ahead in the desired -Z direction");
  Check(left.target.x < 0, "left step lands on the left side");
  Near(left.target.y, floor.y, "left step lands on the flat floor");
  Near(right.target.x, m.foot[1].position.x, "stance foot holds its measured position");

  const f32 step_len = Length(left.target - contacts.support_center);
  Check(step_len <= params.max_step_length + 1e-4f, "step length never exceeds max_step_length");
  Check(Finite(left.target) && Finite(left.swing_position) && Finite(left.swing_velocity) &&
            Finite(left.foot_orientation),
        "swing outputs finite");
  Check(Finite(right.target) && Finite(right.foot_orientation), "stance outputs finite");

  // Alternation: phase 0.25 -> right foot swings, left plants, opposite side.
  GaitState g2 = g;
  g2.phase = 0.25f;
  FootstepPlanner planner2;
  planner2.Update(m, contacts, g2, intent, params, FlatProbe, &floor, dt);
  Check(planner2.plan(1).swinging, "right foot swings at phase 0.25");
  Check(!planner2.plan(0).swinging, "left foot plants at phase 0.25");
  Check(planner2.plan(1).target.x > 0, "right step lands on the right side");

  // Too-steep terrain: rejected and shortened back to the lift-off point.
  FootstepPlanner planner3;
  planner3.Update(m, contacts, g, intent, params, SteepProbe, nullptr, dt);
  const FootPlan& steep = planner3.plan(0);
  Check(steep.rejected == StepReject::kTooSteep, "steep terrain sets kTooSteep");
  Near(steep.target.x, m.foot[0].position.x, "steep step lands at swing start x", 1e-3f);
  Near(steep.target.z, m.foot[0].position.z, "steep step lands at swing start z", 1e-3f);
  Check(Finite(steep.target) && Finite(steep.swing_position), "steep outputs finite");

  // Missing terrain: rejected as kNoGroundHit, stepped in place.
  FootstepPlanner planner4;
  planner4.Update(m, contacts, g, intent, params, NoHitProbe, nullptr, dt);
  const FootPlan& air = planner4.plan(0);
  Check(air.rejected == StepReject::kNoGroundHit, "no ground sets kNoGroundHit");
  Near(air.target.x, m.foot[0].position.x, "no-ground step lands at swing start x", 1e-3f);
  Check(Finite(air.target) && Finite(air.swing_position) && Finite(air.swing_velocity),
        "no-ground outputs finite");
}

// --- SolveLegIk / BuildWholeBodyTargets
// ------------------------------------

// Rotation angle of a quaternion delta from identity (radians, [0,pi]).
f32 QuatAngle(const Quat& q) {
  const Quat n = Normalize(q);
  return 2.0f * std::acos(std::abs(n.w) > 1.0f ? 1.0f : std::abs(n.w));
}

// Independent forward kinematics matching the module's chain convention: the
// solved hip aims the thigh (bind -Y), the knee is a -X hinge by `knee_flexion`,
// and the ankle carries the foot (bind offset -Y*foot_height) whose bind normal
// is +Y. Returns the sole centre relative to the hip pivot.
Vec3 LegForwardKinematics(const LegIkResult& r, f32 upper, f32 lower, f32 foot_height) {
  const Quat q_knee = QuatFromAxisAngle({-1, 0, 0}, r.knee_flexion);
  const Vec3 knee_pos = Rotate(r.hip, {0, -upper, 0});
  const Quat chain_lower = r.hip * q_knee;
  const Vec3 ankle_pos = knee_pos + Rotate(chain_lower, {0, -lower, 0});
  const Quat chain_foot = chain_lower * r.ankle;
  return ankle_pos + Rotate(chain_foot, {0, -foot_height, 0});
}

// Ankle-joint position (hip + knee only), relative to the hip pivot. This is the
// pure 2-bone solve, independent of the ankle's sole-normal cone clamp, so it is
// the rigorous ground truth for the hip-rotation construction.
Vec3 LegForwardAnkle(const LegIkResult& r, f32 upper, f32 lower) {
  const Quat q_knee = QuatFromAxisAngle({-1, 0, 0}, r.knee_flexion);
  const Vec3 knee_pos = Rotate(r.hip, {0, -upper, 0});
  return knee_pos + Rotate(r.hip * q_knee, {0, -lower, 0});
}

// Hand-built rig geometry (never call BipedRig::Build here — that needs Jolt and
// belongs to the concurrent agent's test). Straight-leg reach (hip->sole) is
// upper + lower + foot_height = 0.904 m, so pelvis_height = leg_length = 0.904
// puts the soles flat on the ground with the legs essentially straight at bind.
BipedRig MakeRig() {
  BipedRig rig;
  rig.upper_leg_length = 0.45f;
  rig.lower_leg_length = 0.40f;
  rig.foot_height = 0.054f;
  rig.leg_length = 0.904f;
  rig.pelvis_height = 0.904f;
  rig.hip_local[0] = {-0.12f, 0, 0};  // hip_width 0.24 / 2
  rig.hip_local[1] = {0.12f, 0, 0};
  return rig;
}

void TestLegIkForwardKinematics() {
  const f32 upper = 0.45f, lower = 0.40f, foot_height = 0.054f;
  const Vec3 flat_normal{0, 1, 0};

  // (a) Rigorous hip-rotation ground truth across a wide reachable grid: the
  // hip + knee solve must place the ANKLE joint exactly on the requested ankle
  // point (= target + normal*foot_height), regardless of the ankle's sole-normal
  // cone clamp. This is the FK verification of the hip construction.
  int reachable = 0;
  for (f32 x = -0.15f; x <= 0.151f; x += 0.15f) {
    for (f32 y = -0.60f; y >= -0.801f; y -= 0.10f) {
      for (f32 z = -0.25f; z <= 0.251f; z += 0.25f) {
        const Vec3 target{x, y, z};
        const Vec3 ankle_target = target + flat_normal * foot_height;
        const LegIkResult r = SolveLegIk(target, flat_normal, upper, lower, foot_height);
        Check(Finite(r.hip) && Finite(r.ankle) && std::isfinite(r.knee_flexion), "leg IK finite");
        Check(r.knee_flexion >= 0.0f, "knee flexion non-negative");
        Check(!r.clamped, "reachable target not clamped");
        const Vec3 ankle = LegForwardAnkle(r, upper, lower);
        Check(Length(ankle - ankle_target) < 1e-3f, "FK ankle reproduces the 2-bone IK target");
        ++reachable;
      }
    }
  }
  Check(reachable >= 10, "at least 10 reachable IK targets verified");

  // (b) Full sole reproduction (through hip + knee + ankle) on near-straight
  // targets, where keeping the foot flat needs less than the 0.5 rad ankle cone
  // so the sole plane is achieved exactly. (A deeply-bent knee would require the
  // heel to lift, which the cone deliberately prevents — see whole_body.cc.)
  const Vec3 near_straight[] = {
      {0.0f, -0.88f, 0.0f},   {0.0f, -0.85f, 0.0f},    {0.0f, -0.83f, 0.0f},
      {0.10f, -0.86f, 0.0f},  {-0.10f, -0.86f, 0.0f},  {0.0f, -0.85f, 0.12f},
      {0.0f, -0.85f, -0.12f}, {0.08f, -0.84f, 0.08f},  {-0.08f, -0.84f, -0.08f},
      {0.10f, -0.85f, 0.08f}, {-0.10f, -0.87f, 0.06f}, {0.0f, -0.90f, 0.0f},
  };
  int sole_ok = 0;
  for (const Vec3& target : near_straight) {
    const LegIkResult r = SolveLegIk(target, flat_normal, upper, lower, foot_height);
    Check(!r.clamped, "near-straight target reachable");
    const Vec3 sole = LegForwardKinematics(r, upper, lower, foot_height);
    Check(Length(sole - target) < 0.02f, "FK sole reproduces the IK target");
    ++sole_ok;
  }
  Check(sole_ok >= 10, "at least 10 full-sole reproductions verified");
}

void TestLegIkUnreachable() {
  const f32 upper = 0.45f, lower = 0.40f, foot_height = 0.054f;
  const Vec3 flat_normal{0, 1, 0};
  // Far beyond upper + lower + foot_height (= 0.904), downward dominant.
  const Vec3 target{0.3f, -2.0f, 0.2f};
  const LegIkResult r = SolveLegIk(target, flat_normal, upper, lower, foot_height);
  Check(r.clamped, "unreachable target clamps");
  Near(r.knee_flexion, 0.0f, "unreachable leg is ~straight", 0.05f);
  const Vec3 sole = LegForwardKinematics(r, upper, lower, foot_height);
  const f32 cos_angle = Dot(Normalize(sole), Normalize(target));
  const f32 angle = std::acos(cos_angle > 1.0f ? 1.0f : cos_angle);
  Check(angle < 2.0f * 3.14159265f / 180.0f, "FK aims within 2deg of the target direction");
}

void TestLegIkDegenerate() {
  const LegIkResult r = SolveLegIk({0, 0, 0}, {0, 1, 0}, 0.45f, 0.40f, 0.054f);
  Check(Finite(r.hip) && Finite(r.ankle) && std::isfinite(r.knee_flexion), "degenerate finite");
  Check(r.clamped, "zero-length target flagged clamped");
}

// Standing measurements: pelvis at bind height above flat ground, both feet
// planted at +-hip_width/2, both supporting.
CharacterMeasurements MakeStanding(const BipedRig& rig, const ControllerParameters& params,
                                   f32 pelvis_y) {
  CharacterMeasurements m;
  m.valid = true;
  m.root_position = {0, pelvis_y, 0};
  m.root_rotation = {0, 0, 0, 1};
  m.com_position = {0, pelvis_y, 0};
  m.com_velocity = {0, 0, 0};
  m.root_angular_velocity = {0, 0, 0};
  m.ground_normal = {0, 1, 0};
  const f32 half = params.hip_width * 0.5f;
  m.foot[0].position = {-half, 0, 0};
  m.foot[1].position = {half, 0, 0};
  m.foot[0].contact_normal = {0, 1, 0};
  m.foot[1].contact_normal = {0, 1, 0};
  m.foot[0].in_contact = true;
  m.foot[1].in_contact = true;
  return m;
}

void MakeStancePlans(const ControllerParameters& params, FootPlan plan[kFootCount]) {
  const f32 half = params.hip_width * 0.5f;
  for (u32 i = 0; i < kFootCount; ++i) {
    plan[i] = FootPlan{};
    plan[i].swinging = false;
    plan[i].foot_orientation = {0, 0, 0, 1};
  }
  plan[0].target = {-half, 0, 0};
  plan[0].swing_position = plan[0].target;
  plan[1].target = {half, 0, 0};
  plan[1].swing_position = plan[1].target;
}

void TestWholeBodyStanding() {
  const ControllerParameters params;
  const BipedRig rig = MakeRig();
  const PhysicalModifiers modifiers;  // strength 1, balance 1, carried_mass 0
  FootPlan plan[kFootCount];
  MakeStancePlans(params, plan);

  GaitState gait;  // speed_ratio 0, phase 0
  ContactEstimate contacts = MakeContacts();  // support_count 2, center at origin
  LocomotionIntent intent;  // zero desired velocity, facing -Z, height 0

  // (a) Perfectly-at-bind standing pose: joints near identity (except the
  // by-design elbow rest flexion), all outputs finite, stance/arm drive scales.
  {
    const CharacterMeasurements m = MakeStanding(rig, params, rig.pelvis_height);
    WholeBodyTargets out;
    BuildWholeBodyTargets(m, contacts, gait, plan, intent, modifiers, params, rig, &out);

    for (u32 j = 0; j < kRigJointCount; ++j) {
      Check(Finite(out.joint_target[j]), "joint target finite");
      Check(std::isfinite(out.joint_drive_scale[j]), "drive scale finite");
    }
    Check(Finite(out.root_assist_force) && Finite(out.root_assist_torque), "root assists finite");

    // Legs, spine and shoulders sit near bind; elbows carry a rest flexion.
    const RigJoint near_bind[] = {RigJoint::kHipL,   RigJoint::kKneeL,  RigJoint::kAnkleL,
                                  RigJoint::kHipR,   RigJoint::kKneeR,  RigJoint::kAnkleR,
                                  RigJoint::kWaist,  RigJoint::kNeck,   RigJoint::kShoulderL,
                                  RigJoint::kShoulderR};
    for (RigJoint j : near_bind)
      Check(QuatAngle(out.joint_target[static_cast<u32>(j)]) < 0.2f, "bind-pose joint near identity");
    Near(QuatAngle(out.joint_target[static_cast<u32>(RigJoint::kElbowL)]), 0.25f,
         "elbow rest flexion 0.25", 1e-3f);

    // Stance-leg drive scales are 1 (strength 1); arms use arm_drive_scale.
    const RigJoint stance[] = {RigJoint::kHipL,  RigJoint::kKneeL, RigJoint::kAnkleL,
                               RigJoint::kHipR,  RigJoint::kKneeR, RigJoint::kAnkleR};
    for (RigJoint j : stance)
      Near(out.joint_drive_scale[static_cast<u32>(j)], 1.0f, "stance drive scale == 1");
    const RigJoint arms[] = {RigJoint::kShoulderL, RigJoint::kShoulderR, RigJoint::kElbowL,
                             RigJoint::kElbowR};
    for (RigJoint j : arms)
      Near(out.joint_drive_scale[static_cast<u32>(j)], params.arm_drive_scale * modifiers.strength,
           "arm drive scale == arm_drive_scale * strength");

    // At the correct height with zero velocity the vertical assist vanishes.
    Near(out.root_assist_force.y, 0.0f, "no vertical assist at correct height", 1.0f);

    // Both plans copied through.
    Near(out.foot[0].target.x, plan[0].target.x, "left plan copied through");
    Near(out.foot[1].target.x, plan[1].target.x, "right plan copied through");
  }

  // (b) Pelvis 5 cm LOW -> positive (upward) vertical assist.
  {
    const CharacterMeasurements m = MakeStanding(rig, params, rig.pelvis_height - 0.05f);
    WholeBodyTargets out;
    BuildWholeBodyTargets(m, contacts, gait, plan, intent, modifiers, params, rig, &out);
    Check(out.root_assist_force.y > 0.0f, "low pelvis produces upward assist");
    Check(Finite(out.root_assist_force), "assist finite when low");
  }

  // (c) No supporting foot -> both root assists are exactly zero.
  {
    ContactEstimate airborne = contacts;
    airborne.support_count = 0;
    const CharacterMeasurements m = MakeStanding(rig, params, rig.pelvis_height - 0.05f);
    WholeBodyTargets out;
    BuildWholeBodyTargets(m, airborne, gait, plan, intent, modifiers, params, rig, &out);
    Near(out.root_assist_force.x, 0.0f, "airborne force x zero");
    Near(out.root_assist_force.y, 0.0f, "airborne force y zero");
    Near(out.root_assist_force.z, 0.0f, "airborne force z zero");
    Near(out.root_assist_torque.x, 0.0f, "airborne torque x zero");
    Near(out.root_assist_torque.y, 0.0f, "airborne torque y zero");
    Near(out.root_assist_torque.z, 0.0f, "airborne torque z zero");
  }
}

void TestWholeBodyLean() {
  const ControllerParameters params;
  const BipedRig rig = MakeRig();
  const PhysicalModifiers modifiers;
  FootPlan plan[kFootCount];
  MakeStancePlans(params, plan);
  GaitState gait;
  const ContactEstimate contacts = MakeContacts();

  const CharacterMeasurements m = MakeStanding(rig, params, rig.pelvis_height);

  LocomotionIntent rest;  // zero desired velocity
  WholeBodyTargets out_rest;
  BuildWholeBodyTargets(m, contacts, gait, plan, rest, modifiers, params, rig, &out_rest);

  LocomotionIntent moving;
  moving.desired_velocity = {0, 0, -3.0f};  // 3 m/s forward from rest
  moving.desired_facing = {0, 0, -1};
  WholeBodyTargets out_move;
  BuildWholeBodyTargets(m, contacts, gait, plan, moving, modifiers, params, rig, &out_move);

  for (u32 j = 0; j < kRigJointCount; ++j)
    Check(Finite(out_move.joint_target[j]), "leaning joint target finite");
  Check(Finite(out_move.root_assist_force) && Finite(out_move.root_assist_torque),
        "leaning assists finite");

  // The forward acceleration lean tips the pelvis frame, so the pelvis-relative
  // waist and hip targets shift away from the zero-intent pose.
  const Quat& waist_rest = out_rest.joint_target[static_cast<u32>(RigJoint::kWaist)];
  const Quat& waist_move = out_move.joint_target[static_cast<u32>(RigJoint::kWaist)];
  Check(QuatAngle(Conjugate(waist_rest) * waist_move) > 0.02f, "lean shifts the waist target");
  const Quat& hip_rest = out_rest.joint_target[static_cast<u32>(RigJoint::kHipL)];
  const Quat& hip_move = out_move.joint_target[static_cast<u32>(RigJoint::kHipL)];
  Check(QuatAngle(Conjugate(hip_rest) * hip_move) > 0.02f, "lean shifts the hip target");

  // A forward velocity error drives a forward planar assist (-Z).
  Check(out_move.root_assist_force.z < 0.0f, "forward velocity error assists forward");
}

}  // namespace

int main() {
  TestWrap01();
  TestGaitFeetOppose();
  TestGaitCycleAndStance();
  TestGaitStartStop();
  TestGaitRampSmooth();
  TestCapturePoint();
  TestSwingTrajectory();
  TestFootstepPlanner();
  TestLegIkForwardKinematics();
  TestLegIkUnreachable();
  TestLegIkDegenerate();
  TestWholeBodyStanding();
  TestWholeBodyLean();

  if (failures != 0) {
    std::fprintf(stderr, "locomotion_math_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("locomotion_math_test: PASS\n");
  return 0;
}
