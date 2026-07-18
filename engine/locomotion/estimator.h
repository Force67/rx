#ifndef RX_LOCOMOTION_ESTIMATOR_H_
#define RX_LOCOMOTION_ESTIMATOR_H_

// State estimation for the physics-first locomotion controller
// (docs/LOCOMOTION.md). Both classes MEASURE the simulated body every fixed
// step; neither assumes the previous plan actually happened. Everything reads
// through the physics adapter surface, so there are no Jolt includes here.

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "locomotion/rig.h"
#include "locomotion/types.h"
#include "physics/physics_world.h"

namespace rx::locomotion {

// Measures the simulated body each fixed step: root pose/velocity from the
// pelvis, mass-weighted centre of mass and COM velocity over all 13 bodies
// (plus any carried mass), per-foot sole position / velocity / contact, blended
// ground normal and an estimated crown height.
class RX_LOCOMOTION_EXPORT StateEstimator {
 public:
  void Measure(const physics::PhysicsWorld& physics, const BipedRig& rig,
               const PhysicalModifiers& modifiers, CharacterMeasurements* out) const;
};

// Classifies each foot (supporting / sliding / swinging / unconfirmed) with
// hysteresis so a single noisy frame cannot flip it, and owns the per-phase
// timers and the support region.
class RX_LOCOMOTION_EXPORT ContactEstimator {
 public:
  void Update(const CharacterMeasurements& m, f32 dt);
  const ContactEstimate& estimate() const { return estimate_; }
  void Reset();

 private:
  ContactEstimate estimate_{};
  // Per foot: the raw class proposed for the pending change and how many
  // consecutive ticks it has held. A change commits once the count reaches the
  // required dwell (2 to enter supporting, 3 to leave it, 2 otherwise).
  FootPhase pending_phase_[kFootCount] = {FootPhase::kUnconfirmed, FootPhase::kUnconfirmed};
  u32 pending_ticks_[kFootCount] = {0, 0};
};

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_ESTIMATOR_H_
