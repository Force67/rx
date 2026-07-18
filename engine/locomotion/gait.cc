// GaitClock implementation (docs/LOCOMOTION.md §gait). See gait.h for the
// convention and invariants; everything here is branch-clean deterministic
// math with finite outputs for dt = 0.

#include "locomotion/gait.h"

#include <cmath>

#include "locomotion/internal_math.h"

namespace rx::locomotion {
using namespace internal;
namespace {

// Time constant (s) for the gait-frequency lag so the stride rate doesn't step.
constexpr f32 kSpeedRatioTau = 0.15f;

}  // namespace

f32 Wrap01(f32 x) {
  if (!std::isfinite(x)) return 0;
  x -= std::floor(x);
  if (x < 0) x += 1;   // guard tiny-negative rounding
  if (x >= 1) x -= 1;  // guard rounding up to exactly 1
  return x;
}

void GaitClock::Update(const CharacterMeasurements& measurements, const LocomotionIntent& intent,
                       const ControllerParameters& params, bool need_step, f32 dt) {
  const f32 desired_speed = PlanarLength(intent.desired_velocity);
  const f32 measured_speed = PlanarLength(measurements.com_velocity);

  // Smooth the normalized speed toward its target so the phase rate ramps.
  const f32 target_ratio =
      params.run_speed > 0 ? Clampf(desired_speed / params.run_speed, 0, 1) : 0;
  state_.speed_ratio += (target_ratio - state_.speed_ratio) * SmoothingAlpha(dt, kSpeedRatioTau);
  state_.speed_ratio = Clampf(state_.speed_ratio, 0, 1);

  state_.phase_rate =
      Lerpf(params.walk_stride_frequency, params.run_stride_frequency, state_.speed_ratio);
  state_.stance_fraction =
      Lerpf(params.stance_fraction_walk, params.stance_fraction_run, state_.speed_ratio);

  // Start/stop logic. Stepping starts as soon as the character wants to move
  // (or balance forces a step). It stops only once the body has actually slowed
  // and the phase reaches a double-support point, so a stopping character
  // completes its current step instead of snapping a foot down.
  const bool want_step = desired_speed > 0.1f * params.walk_speed || need_step;
  if (want_step) {
    state_.stepping = true;
  } else if (state_.stepping) {
    const bool measured_slow = measured_speed < 0.25f;
    const bool both_stance = InStance(state_, 0) && InStance(state_, 1);
    if (measured_slow && !need_step && both_stance) state_.stepping = false;
  }

  // Advance (or freeze) the phase.
  if (state_.stepping) {
    f32 dphase = state_.phase_rate * dt;
    dphase = Clampf(dphase, 0, 0.5f);  // never leap over a whole half cycle
    state_.phase = Wrap01(state_.phase + dphase);
  }
}

void GaitClock::Reset() { state_ = GaitState{}; }

f32 GaitClock::FootPhase(const GaitState& s, u32 foot) {
  return foot == 0 ? s.phase : Wrap01(s.phase + 0.5f);
}

bool GaitClock::InStance(const GaitState& s, u32 foot) {
  return FootPhase(s, foot) < s.stance_fraction;
}

f32 GaitClock::SwingProgress(const GaitState& s, u32 foot) {
  const f32 fp = FootPhase(s, foot);
  if (fp < s.stance_fraction) return 0;  // still planted
  const f32 swing_span = s.stance_fraction < 1 ? 1.0f - s.stance_fraction : 1e-4f;
  return Clampf((fp - s.stance_fraction) / swing_span, 0, 1);
}

}  // namespace rx::locomotion
