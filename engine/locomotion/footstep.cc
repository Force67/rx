// Footstep planning implementation (docs/LOCOMOTION.md §footstep). See
// footstep.h for conventions and finiteness guarantees. Pure math: terrain is
// touched only through the caller's downward probe.

#include "locomotion/footstep.h"

#include <cmath>

#include "locomotion/gait.h"

namespace rx::locomotion {
namespace {

constexpr f32 kGravity = 9.81f;      // positive magnitude, m/s^2
constexpr f32 kMaxFootTilt = 0.35f;  // rad the sole may tilt off the vertical
const Vec3 kUp{0, 1, 0};

f32 Clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : (x > hi ? hi : x); }

f32 Lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// Drop the vertical component of a world vector.
Vec3 Planar(const Vec3& v) { return {v.x, 0, v.z}; }

// Clamp a vector's length to `max_len` (no-op below it; safe for the zero vec).
Vec3 ClampLength(const Vec3& v, f32 max_len) {
  const f32 len = Length(v);
  if (len <= max_len || len <= 0) return v;
  return v * (max_len / len);
}

// Planar facing direction: prefer intent, fall back to the root's forward, then
// to -Z, so a zero/degenerate facing never poisons the lateral offset.
Vec3 ResolveFacing(const LocomotionIntent& intent, const Quat& root_rotation) {
  Vec3 f = Planar(intent.desired_facing);
  if (Length(f) > 1e-4f) return Normalize(f);
  f = Planar(Rotate(root_rotation, Vec3{0, 0, -1}));
  if (Length(f) > 1e-4f) return Normalize(f);
  return {0, 0, -1};
}

// World foot orientation: yaw from the planar facing, plus a tilt that aligns
// the sole's up to `normal` (clamped to kMaxFootTilt so a steep normal never
// rolls the ankle past its limit).
Quat FootOrientation(const Vec3& normal, const Vec3& facing) {
  Vec3 n = Length(normal) > 1e-5f ? Normalize(normal) : kUp;
  const f32 angle = std::acos(Clampf(n.y, -1, 1));
  Quat tilt{0, 0, 0, 1};
  if (angle > 1e-4f) {
    Vec3 axis = Cross(kUp, n);
    if (Length(axis) > 1e-5f)
      tilt = QuatFromAxisAngle(axis, angle < kMaxFootTilt ? angle : kMaxFootTilt);
  }
  const Quat yaw = QuatBetween(Vec3{0, 0, -1}, facing);
  return tilt * yaw;
}

}  // namespace

Vec3 CapturePoint(const Vec3& com_position, const Vec3& com_velocity, f32 gravity, f32 com_height) {
  const f32 h = com_height < 0.1f ? 0.1f : com_height;
  const f32 omega = std::sqrt((gravity > 0 ? gravity : kGravity) / h);
  if (!(omega > 0)) return com_position;
  return com_position + com_velocity * (1.0f / omega);
}

Vec3 SwingPosition(const Vec3& start, const Vec3& target, f32 step_height, f32 t) {
  const f32 s = t * t * (3.0f - 2.0f * t);  // smoothstep horizontal blend
  Vec3 p = Lerp(start, target, s);
  p.y += step_height * 4.0f * t * (1.0f - t);  // parabolic lift, apex at t=0.5
  return p;
}

Vec3 SwingVelocity(const Vec3& start, const Vec3& target, f32 step_height, f32 t, f32 t_rate) {
  const f32 ds = 6.0f * t * (1.0f - t);  // d/dt smoothstep
  Vec3 v = (target - start) * ds;
  v.y += step_height * 4.0f * (1.0f - 2.0f * t);  // d/dt parabolic lift
  return v * t_rate;
}

void FootstepPlanner::Update(const CharacterMeasurements& m, const ContactEstimate& contacts,
                             const GaitState& gait, const LocomotionIntent& intent,
                             const ControllerParameters& params, GroundProbeFn probe,
                             void* probe_context, f32 dt) {
  (void)dt;  // the planner is memoryless per tick beyond the lift-off latch
  const Vec3 facing = ResolveFacing(intent, m.root_rotation);
  const Vec3 right = Normalize(Cross(facing, kUp));  // character right (+X at yaw 0)
  const f32 cos_slope = std::cos(params.max_ground_slope);

  for (u32 foot = 0; foot < kFootCount; ++foot) {
    FootPlan& p = plan_[foot];
    const bool swinging = gait.stepping && !GaitClock::InStance(gait, foot);

    if (!swinging) {
      // Stance (or parked): hold the measured contact.
      const Vec3 foot_pos = m.foot[foot].position;
      p.swinging = false;
      p.target = foot_pos;
      p.swing_position = foot_pos;
      p.swing_velocity = {0, 0, 0};
      p.swing_progress = 0;
      p.rejected = StepReject::kNone;
      p.foot_orientation = FootOrientation(m.foot[foot].contact_normal, facing);
      was_swinging_[foot] = false;
      continue;
    }

    // Lift-off edge: latch where the sole left the ground.
    if (!was_swinging_[foot]) {
      swing_start_[foot] = m.foot[foot].position;
      was_swinging_[foot] = true;
    }

    // Raw landing target, assembled in the ground plane (y from the probe).
    const Vec3 desired_v = Planar(intent.desired_velocity);
    const Vec3 com_v = Planar(m.com_velocity);
    const Vec3 lateral = right * (0.5f * params.hip_width * (foot == 1 ? 1.0f : -1.0f));

    const f32 com_height = m.com_position.y - contacts.support_center.y;
    const Vec3 capture = CapturePoint(m.com_position, m.com_velocity, kGravity, com_height);
    Vec3 capture_off = Planar(capture - contacts.support_center) * params.capture_gain;
    capture_off = ClampLength(capture_off, params.recovery_margin * 2.0f);

    Vec3 target = Vec3{m.root_position.x, contacts.support_center.y, m.root_position.z} +
                  desired_v * params.lookahead_time + lateral +
                  (desired_v - com_v) * params.step_velocity_gain + capture_off;

    // Clamp the step geometry relative to the support center.
    Vec3 step = target - contacts.support_center;
    const f32 lat = Dot(step, right);
    step += right * (Clampf(lat, -params.max_step_width, params.max_step_width) - lat);
    step = ClampLength(step, params.max_step_length);
    target = contacts.support_center + step;

    // Project onto the terrain, shortening toward the lift-off point on a
    // reject: halve once, then step in place. plan.rejected is the LAST reason.
    GroundHit hit{};
    bool got = false;
    auto validate = [&](const Vec3& t) -> StepReject {
      const Vec3 start = t + kUp * params.max_step_up;
      const f32 depth = params.max_step_up + params.max_step_down;
      got = probe && probe(probe_context, start, depth, &hit);
      if (!got) return StepReject::kNoGroundHit;
      if (hit.normal.y < cos_slope) return StepReject::kTooSteep;
      if (hit.position.y - contacts.support_center.y > params.max_step_up)
        return StepReject::kTooHigh;
      return StepReject::kNone;
    };

    StepReject reject = validate(target);
    if (reject != StepReject::kNone) {
      target = Lerp(target, swing_start_[foot], 0.5f);
      reject = validate(target);
      if (reject != StepReject::kNone) {
        target = swing_start_[foot];
        reject = validate(target);
      }
    }
    // Vertical: land on the probed ground, or hold the lift-off height.
    target.y = got ? hit.position.y : swing_start_[foot].y;
    const Vec3 land_normal = got ? hit.normal : kUp;

    // Live swing pose.
    const f32 t = GaitClock::SwingProgress(gait, foot);
    const f32 swing_span = gait.stance_fraction < 1 ? 1.0f - gait.stance_fraction : 1e-4f;
    const f32 t_rate = gait.phase_rate / swing_span;
    Vec3 sp = SwingPosition(swing_start_[foot], target, params.step_height, t);
    Vec3 sv = SwingVelocity(swing_start_[foot], target, params.step_height, t, t_rate);

    // Near landing: settle onto the ground height and taper the descent so the
    // sole doesn't slam.
    if (t > 0.8f) {
      const f32 b = (t - 0.8f) / 0.2f;  // 0 at t=0.8 .. 1 at t=1
      sp.y = Lerpf(sp.y, target.y, b);
      sv.y *= (1.0f - t) / 0.2f;
    }

    p.swinging = true;
    p.target = target;
    p.swing_position = sp;
    p.swing_velocity = sv;
    p.swing_progress = t;
    p.foot_orientation = FootOrientation(land_normal, facing);
    p.rejected = reject;
  }
}

void FootstepPlanner::Reset() {
  for (u32 foot = 0; foot < kFootCount; ++foot) {
    plan_[foot] = FootPlan{};
    swing_start_[foot] = Vec3{};
    was_swinging_[foot] = false;
  }
}

}  // namespace rx::locomotion
