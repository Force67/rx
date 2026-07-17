#include "character/jetpack.h"

#include <algorithm>
#include <cmath>

#include "ecs/world.h"

namespace rx::character {
namespace {

f32 Clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// ln(10): a first-order lag with time constant tau = spool_time / kLn10 reaches
// 90% (1 - e^{-t/tau} = 0.9) exactly at t = spool_time, so `spool_time` reads as
// the ~90% rise time.
constexpr f32 kLn10 = 2.302585093f;

}  // namespace

void StepJetpacks(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0.0f) return;

  world.Each<JetpackDesc, JetpackInput, JetpackState, CharacterMovementSettings, CharacterState,
             CharacterIntent>(
      [&](ecs::Entity, JetpackDesc& d, JetpackInput& in, JetpackState& st,
          CharacterMovementSettings& move, CharacterState& cs, CharacterIntent& intent) {
        const bool grounded = cs.grounded;  // last step's result (StepCharacters runs after us)

        // --- thrust demand + spool lag ---------------------------------------
        // Burn only while enabled, held AND fuel remains; empty tank = dead stick.
        const bool want = in.enabled && in.thrust && st.fuel > 0.0f;
        const f32 demand = want ? 1.0f : 0.0f;
        const f32 tau = d.spool_time > 1e-4f ? d.spool_time / kLn10 : 0.0f;
        const f32 a = tau > 0.0f ? 1.0f - std::exp(-dt / tau) : 1.0f;
        st.thrust = Clamp01(st.thrust + (demand - st.thrust) * a);

        // --- fuel: drain by actual thrust; refuel grounded + idle only --------
        const f32 cap = std::max(d.fuel_capacity_s, 1e-3f);
        if (st.thrust > 1e-3f) {
          st.fuel -= st.thrust * (dt / cap);  // full thrust empties the tank in `cap` s
        } else if (grounded && d.refuel_rate > 0.0f) {
          st.fuel += d.refuel_rate * dt;  // tank fractions per grounded second
        }
        st.fuel = Clamp01(st.fuel);
        st.burning = st.thrust > 0.02f;
        st.refueling = grounded && !want && d.refuel_rate > 0.0f && st.fuel < 1.0f;

        // --- thrust -> acceleration (the seam StepCharacters integrates) ------
        // Vertical: TWR * gravity, so it competes with weight and only climbs
        // when thrust_to_weight * thrust > 1. Lateral: along the horizontal move
        // intent, scaled by thrust — the in-air lean that beats free-fall drift.
        const f32 g = std::max(move.gravity, 0.0f);
        Vec3 accel{0.0f, st.thrust * d.thrust_to_weight * g, 0.0f};
        const Vec3 mh{intent.move.x, 0.0f, intent.move.z};
        const f32 mlen = std::sqrt(mh.x * mh.x + mh.z * mh.z);
        if (mlen > 1e-4f) {
          const f32 s = (st.thrust * d.lateral_accel) / mlen;
          accel.x += mh.x * s;
          accel.z += mh.z * s;
        }
        if (std::isfinite(accel.x) && std::isfinite(accel.y) && std::isfinite(accel.z))
          intent.external_acceleration += accel;  // compose (StepCharacters clears it)
      });
}

}  // namespace rx::character
