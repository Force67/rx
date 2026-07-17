#ifndef RX_CHARACTER_JETPACK_H_
#define RX_CHARACTER_JETPACK_H_

#include "character/character.h"
#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::ecs {
class World;
}

// A player jetpack layered on the character controller, in the module's idiom:
// plain-data components plus one free-function system staged per fixed step. It
// owns no physics body — the character controller is a kinematic, velocity-based
// mover (there is no mass to push), so the pack works in ACCELERATION units and
// injects its thrust through CharacterIntent::external_acceleration, the seam the
// controller integrates before it consumes the velocity. Because that seam
// competes with gravity honestly and the controller's ground clamp still stops
// downward motion, a jetpack climbs, hovers-with-finesse and dead-sticks without
// ever bypassing the existing fall/land handling.
//
// Staging: run StepJetpacks(world, dt) BEFORE character::StepCharacters(world,
// physics, dt) each fixed step. StepJetpacks reads last step's grounded flag
// (for refuel) and this step's move intent (for the lateral thrust vector) and
// writes CharacterIntent::external_acceleration; StepCharacters then folds it in
// and clears it.
namespace rx::character {

// Jetpack tuning (a component; plain data, like CharacterMovementSettings). Five
// knobs: thrust, spool, fuel, refuel, air-control boost.
struct JetpackDesc {
  // Full-thrust vertical acceleration as a multiple of the character's gravity
  // (CharacterMovementSettings::gravity). >1 climbs, =1 exactly hovers, <1 only
  // softens a fall. Kept modest (~1.3-1.6) so lift-off is brisk, not violent.
  // There is NO auto-hover: matching thrust to weight to hang still is left to
  // the player's finesse (intended — see the demo).
  f32 thrust_to_weight = 1.45f;
  // ~90% thrust rise time, seconds: the actual thrust lags the demand through a
  // first-order spool so a stab of the button does not snap to full thrust.
  f32 spool_time = 0.3f;
  // Tank size in seconds of full-throttle burn. Fuel drains proportional to the
  // ACTUAL (spooled) thrust, so partial throttle lasts proportionally longer.
  f32 fuel_capacity_s = 4.0f;
  // Refill rate as tank-fractions per second, applied ONLY while grounded and
  // not thrusting (0.5 => a full tank in 2 s on the ground). Airborne never
  // refuels. 0 disables refuelling (a one-shot tank).
  f32 refuel_rate = 0.5f;
  // Max horizontal thrust-vector authority, m/s^2, scaled by the actual thrust
  // and applied along the move intent: leaning into WASD in the air accelerates
  // faster with the pack burning than a free-fall drift would. Set decisively
  // above the controller's own airborne locomotion authority (ground_acceleration
  // * air_control, ~15 m/s^2 with engine defaults) so the pack's push wins the
  // tug-of-war against the gait-speed cap instead of being clawed back to it.
  f32 lateral_accel = 24.0f;
};

// Per-step control (a component; the game writes it each fixed step).
struct JetpackInput {
  bool enabled = false;  // pack equipped and switched on (a toggle in the demo)
  bool thrust = false;   // hold-to-burn this step
};

// Telemetry (a component), for HUD and audio.
struct JetpackState {
  f32 thrust = 0.0f;      // 0..1 actual, spool-filtered
  f32 fuel = 1.0f;        // 0..1 remaining
  bool burning = false;   // producing meaningful thrust this step
  bool refueling = false; // grounded, idle and topping up
};

// Stage the jetpack for every entity carrying the full set (JetpackDesc,
// JetpackInput, JetpackState) alongside the character components
// (CharacterMovementSettings, CharacterState, CharacterIntent). Spools the
// thrust toward demand, drains/refuels the tank (refuel grounded-only), and
// writes the vertical + lateral thrust into CharacterIntent::external_acceleration.
// An empty tank forces thrust to zero (dead stick → normal gravity fall). Run
// BEFORE StepCharacters each fixed step.
RX_CHARACTER_EXPORT void StepJetpacks(ecs::World& world, f32 dt);

}  // namespace rx::character

#endif  // RX_CHARACTER_JETPACK_H_
