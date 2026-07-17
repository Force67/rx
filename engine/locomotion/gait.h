#ifndef RX_LOCOMOTION_GAIT_H_
#define RX_LOCOMOTION_GAIT_H_

// GaitClock: the continuous phase generator of docs/LOCOMOTION.md §gait. Gait
// is NOT a clip or a state machine — it is one scalar phase in [0,1) advanced
// every fixed step at a speed-dependent rate. The left foot keys `phase`, the
// right foot keys Wrap01(phase + 0.5); a foot is in stance while its foot-phase
// is below the (speed-blended) stance fraction, otherwise in swing. Walk and
// run are only scalar regions of the same continuous curve.
//
// Conventions: right-handed, +Y up, facing yaw 0 looks down -Z. Metres, m/s,
// radians, cycles/second. Pure deterministic math: no allocation, no statics,
// no global reads; every output stays finite for dt = 0.

#include "core/export.h"
#include "core/types.h"
#include "locomotion/types.h"

namespace rx::locomotion {

// Wraps `x` into [0,1). Handles negatives and values >= 1 (exactly 1 -> 0).
RX_LOCOMOTION_EXPORT f32 Wrap01(f32 x);

class RX_LOCOMOTION_EXPORT GaitClock {
 public:
  // Advances the phase toward `intent` for `dt` seconds. `need_step` forces
  // stepping even at zero desired speed (the balance controller sets it when
  // the capture point leaves the support region). Never advances the phase by
  // more than 0.5 in one call, and freezes rather than snaps when stopping so a
  // decelerating character finishes its current step at a double-support point.
  void Update(const CharacterMeasurements& measurements, const LocomotionIntent& intent,
              const ControllerParameters& params, bool need_step, f32 dt);
  const GaitState& state() const { return state_; }
  void Reset();

  // Foot-phase helpers (foot: 0 = left, 1 = right).
  static f32 FootPhase(const GaitState& s, u32 foot);
  static bool InStance(const GaitState& s, u32 foot);
  // 0..1 progress through the swing interval; 0 while in stance.
  static f32 SwingProgress(const GaitState& s, u32 foot);

 private:
  GaitState state_{};
};

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_GAIT_H_
