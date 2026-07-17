#ifndef RX_PHYSICS_VEHICLE_PROFILES_H_
#define RX_PHYSICS_VEHICLE_PROFILES_H_

#include "core/export.h"
#include "physics/physics_world.h"

namespace rx::physics {

// GTA-style handling presets: fully tuned PhysicsWorld::VehicleDesc values so a
// truck handles like a truck and a sports car like a sports car straight out of
// CreateVehicle. Each function returns a complete desc (chassis dims, mass,
// wheel geometry, drivetrain, engine, gearbox, suspension, tyres, aero and the
// handling-profile extensions). One line per preset states the intended
// signature; the measured orderings are proven in test/handling_test.cc.
//
// Units follow VehicleDesc: metres/kg/s, +Z forward, +Y up. The presets differ
// in wheelbase/track/radius and chassis half-extents so a demo scaling its
// visuals from the desc reads a believably different vehicle each time.

// Rear-wheel drive, low centre of mass, stiff springs and a strong front anti-
// roll bar, quick steering with a hard high-speed fade, sticky tyres and real
// downforce: eager turn-in, planted at speed, ~1300 kg. Quick to rev, short
// gears. The benchmark for 0-100 and braking.
RX_PHYSICS_EXPORT PhysicsWorld::VehicleDesc SportsCarProfile();

// Rear-wheel drive with a big low-end torque hit and a rear grip a notch below
// the front, softer rear springs and modest anti-roll: tail-happy, lights up
// the rears on throttle, ~1650 kg. Fast in a straight line, loose in a bend.
RX_PHYSICS_EXPORT PhysicsWorld::VehicleDesc MuscleCarProfile();

// Front-wheel drive, economical torque, a built-in understeer bias (front grip
// below rear so the nose washes wide under power), soft-ish and light
// (~1150 kg). Safe, forgiving, nose-led.
RX_PHYSICS_EXPORT PhysicsWorld::VehicleDesc HatchbackProfile();

// All-wheel drive split 40/60 front/rear, a tall centre of mass (little CoM
// drop), soft long-travel suspension and mild anti-roll, all-terrain tyres that
// keep more grip on dirt/grass, ~2100 kg. Sure-footed launch, leans in corners.
RX_PHYSICS_EXPORT PhysicsWorld::VehicleDesc SuvProfile();

// Rear-wheel drive, high CG over a narrow-ish track, slow steering and a soft
// tall body that rolls a lot; `cargo_load` (0..1) adds mass and shifts the CoM
// rearward and up (empty ~2000 kg, laden ~2400 kg). Ponderous, top-heavy.
RX_PHYSICS_EXPORT PhysicsWorld::VehicleDesc VanProfile(f32 cargo_load = 0.0f);

// Heavy tractor unit (~8500 kg) with enormous torque geared very tall, very
// slow steering, weak per-kilogram brakes (long stops), a high CG and
// pronounced roll, top speed capped by the gearing. Unstoppable, unturnable.
RX_PHYSICS_EXPORT PhysicsWorld::VehicleDesc SemiTruckProfile();

}  // namespace rx::physics

#endif  // RX_PHYSICS_VEHICLE_PROFILES_H_
