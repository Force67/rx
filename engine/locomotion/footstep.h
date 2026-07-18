#ifndef RX_LOCOMOTION_FOOTSTEP_H_
#define RX_LOCOMOTION_FOOTSTEP_H_

// Footstep planning (docs/LOCOMOTION.md §footstep). Each fixed step every foot
// gets a plan: stance feet hold their measured contact, swing feet get a
// terrain-projected, geometry-clamped landing target plus a live analytic swing
// pose (smoothstep horizontal blend + parabolic lift). The landing target
// combines desired velocity look-ahead, a lateral stance offset, a velocity
// error term, and a linear-inverted-pendulum capture-point correction.
//
// The math here is deliberately physics-free: terrain is sampled only through a
// caller-supplied downward probe (which wraps a physics raycast), so the whole
// module is pure and unit-testable. Conventions: right-handed, +Y up, facing
// yaw 0 looks down -Z. Metres, m/s, radians. All outputs stay finite for
// degenerate inputs (zero vectors, zero COM height).

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "locomotion/types.h"

namespace rx::locomotion {

// A downward terrain sample. `position` is the hit point (world), `normal` the
// surface normal (world, +Y up for flat ground).
struct GroundHit {
  Vec3 position;
  Vec3 normal{0, 1, 0};
};

// Terrain probe supplied by the controller (wraps a physics raycast). Probes
// straight down from `probe_start` for up to `max_depth` metres; returns true
// and fills `*out` on a hit, false when nothing is within reach. `context` is
// the opaque pointer passed to FootstepPlanner::Update — no std::function, no
// captured state, so the planner stays allocation- and heap-free.
using GroundProbeFn = bool (*)(void* context, const Vec3& probe_start, f32 max_depth,
                               GroundHit* out);

// Linear-inverted-pendulum capture point: the ground support location that
// would arrest the current COM motion. `gravity` is the positive magnitude
// (9.81). `com_height` is clamped to >= 0.1 m inside the pendulum frequency so
// a degenerate height never divides by zero. Zero velocity returns
// com_position.
RX_LOCOMOTION_EXPORT Vec3 CapturePoint(const Vec3& com_position, const Vec3& com_velocity,
                                       f32 gravity, f32 com_height);

// Analytic swing trajectory, t in [0,1]: smoothstep horizontal blend from
// `start` to `target` plus a parabolic lift of `step_height` above the
// start->target chord (apex at t = 0.5).
RX_LOCOMOTION_EXPORT Vec3 SwingPosition(const Vec3& start, const Vec3& target, f32 step_height,
                                        f32 t);
// Time derivative of SwingPosition w.r.t. normalized t, scaled by `t_rate`
// (1/s), i.e. the world-space sole velocity along the swing.
RX_LOCOMOTION_EXPORT Vec3 SwingVelocity(const Vec3& start, const Vec3& target, f32 step_height,
                                        f32 t, f32 t_rate);

// Plans both feet each tick. Stateless across characters (one instance per
// character); no per-tick allocation.
class RX_LOCOMOTION_EXPORT FootstepPlanner {
 public:
  void Update(const CharacterMeasurements& m, const ContactEstimate& contacts,
              const GaitState& gait, const LocomotionIntent& intent,
              const ControllerParameters& params, GroundProbeFn probe, void* probe_context, f32 dt);
  const FootPlan& plan(u32 foot) const { return plan_[foot]; }
  void Reset();

 private:
  FootPlan plan_[kFootCount]{};
  Vec3 swing_start_[kFootCount]{};  // sole position captured at lift-off
  bool was_swinging_[kFootCount] = {false, false};
};

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_FOOTSTEP_H_
