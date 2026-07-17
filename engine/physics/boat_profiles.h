#ifndef RX_PHYSICS_BOAT_PROFILES_H_
#define RX_PHYSICS_BOAT_PROFILES_H_

#include "physics/boat.h"

namespace rx::physics {

// Boat-type presets: fully tuned BoatDesc values, mirroring vehicle_profiles'
// car presets, so a dinghy handles like a dinghy and a work barge like a barge
// straight out of the Boat constructor. Each function returns a complete desc
// (hull dims, mass, engine/prop, drag/planing/righting tune and cargo capacity);
// its doc comment states the handling signature in one line. The measured
// orderings (settle draft, top speed, planing, turn rate, stability, freeboard,
// and the visible laden-vs-empty draft split) are proven in
// test/boat_profiles_test.cc.
//
// Units follow BoatDesc: metres/kg/s/newtons, +Z fwd, +Y up. Draft is EMERGENT
// from displacement - a heavier hull over a given footprint settles deeper - so
// the presets differ in mass and footprint to sit at believably different
// waterlines, and SetCargo adds mass at runtime so a laden hull visibly sinks,
// turns and planes accordingly.

// ~3 m, ~220 kg including a small outboard: light and twitchy, low top speed,
// planes early but chop-sensitive; a shallow ballast lever so it capsizes far
// more easily than the bigger hulls.
BoatDesc DinghyProfile();

// The ~6 m default motorboat, tuned: fast, planes hard, agile - the benchmark
// the other profiles are read against (equals the BoatDesc defaults).
BoatDesc SpeedboatProfile();

// Tiny ~2.2 m personal watercraft, ~350 kg: extreme thrust-to-weight so it
// planes almost at once, spins on a dime (short hull, strong rudder, light yaw
// damping), easily flipped but self-rights from the buoyancy grid.
BoatDesc JetskiProfile();

// ~9 m displacement hull, ~6500 kg: heavy, high drag, generous cargo capacity,
// very stable (strong ballast); barely planes even empty.
BoatDesc FishingBoatProfile();

// ~12 m, ~9000 kg: very heavy, huge cargo capacity, enormous draft when laden
// (deck near awash at the structural limit), a slow-spooling big-torque engine,
// wide turning; never planes. The cargo showcase.
BoatDesc WorkBargeProfile();

}  // namespace rx::physics

#endif  // RX_PHYSICS_BOAT_PROFILES_H_
