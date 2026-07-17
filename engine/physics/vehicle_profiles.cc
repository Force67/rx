#include "physics/vehicle_profiles.h"

namespace rx::physics {

namespace {

using Desc = PhysicsWorld::VehicleDesc;

// A punchy petrol torque curve (peak in the mid range), reused by the road
// cars. `low` sets the off-idle fraction so a muscle car can be given more
// low-end shove than a peaky sports engine.
void PetrolCurve(Desc& d, f32 low) {
  d.torque_curve[0] = {0.10f, low};
  d.torque_curve[1] = {0.35f, 0.92f};
  d.torque_curve[2] = {0.55f, 1.00f};
  d.torque_curve[3] = {0.80f, 0.94f};
  // Keep healthy top-end torque: a RWD/AWD car must still make force near the
  // redline or it "pins" at a gear's top - it can neither accelerate further
  // nor upshift (Jolt only upshifts while the engine is still revving up). The
  // FWD hatch is the exception (its own hard-tapered curve below): its driven
  // wheels unload and spin, so there the top-end must be cut instead.
  d.torque_curve[4] = {1.00f, 0.82f};
  d.torque_curve_count = 5;
}

// A torquey, low-revving diesel curve for the van and truck: full torque early,
// still pulling hard up top (same anti-pin reason as the petrol curve).
void DieselCurve(Desc& d) {
  d.torque_curve[0] = {0.10f, 0.88f};
  d.torque_curve[1] = {0.30f, 1.00f};
  d.torque_curve[2] = {0.70f, 0.95f};
  d.torque_curve[3] = {1.00f, 0.80f};
  d.torque_curve_count = 4;
}

}  // namespace

PhysicsWorld::VehicleDesc SportsCarProfile() {
  Desc d;
  d.half_extent = {0.95f, 0.42f, 2.05f};  // low, wide, ~4.1 m long
  d.mass = 1300.0f;
  d.wheel_radius = 0.33f;
  d.wheel_width = 0.28f;
  d.wheel_x = 0.83f;
  d.front_z = 1.28f;
  d.rear_z = -1.28f;
  d.suspension_min = 0.10f;
  d.suspension_max = 0.28f;
  d.com_drop = 0.42f;  // low CG
  d.drivetrain = PhysicsWorld::Drivetrain::kRWD;
  d.limited_slip_ratio = 1.25f;  // tight LSD hooks the rears up out of a corner
  d.max_engine_torque = 1060.0f;
  PetrolCurve(d, 0.55f);
  d.max_rpm = 7600.0f;
  d.min_rpm = 1100.0f;
  d.engine_inertia = 0.30f;  // light flywheel, revs fast
  d.gear_count = 6;
  d.gear_ratios[0] = 3.5f;
  d.gear_ratios[1] = 2.4f;
  d.gear_ratios[2] = 1.8f;
  d.gear_ratios[3] = 1.4f;
  d.gear_ratios[4] = 1.12f;
  d.gear_ratios[5] = 0.92f;
  d.final_drive = 4.7f;
  d.shift_up_rpm = 6400.0f;
  d.shift_down_rpm = 3000.0f;
  d.max_steer_angle = 0.62f;
  d.steer_high_speed_fraction = 0.35f;  // strong fade so it stays planted at speed
  d.steer_fade_speed = 45.0f;
  d.anti_roll_front = 5500.0f;
  d.anti_roll_rear = 4200.0f;
  d.suspension_frequency = 2.6f;  // stiff springs
  d.suspension_damping = 0.55f;
  d.tire_long_friction = 1.45f;  // sticky
  d.tire_lat_friction = 1.5f;
  d.brake_bias_front = 0.62f;
  d.max_brake_torque = 3600.0f;
  d.downforce = 3.0f;
  d.downforce_balance = 0.42f;  // slight rear aero bias for stability
  // traction control off: launch differentiation comes from drivetrain + grip.
  return d;
}

PhysicsWorld::VehicleDesc MuscleCarProfile() {
  Desc d;
  d.half_extent = {0.98f, 0.55f, 2.38f};  // long bonnet, ~4.75 m
  d.mass = 1650.0f;
  d.wheel_radius = 0.36f;
  d.wheel_width = 0.30f;
  d.wheel_x = 0.85f;
  d.front_z = 1.45f;
  d.rear_z = -1.42f;
  d.suspension_min = 0.14f;
  d.suspension_max = 0.36f;
  d.com_drop = 0.36f;
  d.drivetrain = PhysicsWorld::Drivetrain::kRWD;
  d.limited_slip_ratio = 1.6f;
  d.max_engine_torque = 660.0f;  // big low-end shove
  PetrolCurve(d, 0.72f);
  d.max_rpm = 6000.0f;
  d.min_rpm = 900.0f;
  d.engine_inertia = 0.55f;  // heavy crank, lazier revs
  d.gear_count = 5;
  d.gear_ratios[0] = 2.9f;
  d.gear_ratios[1] = 1.9f;
  d.gear_ratios[2] = 1.35f;
  d.gear_ratios[3] = 1.05f;
  d.gear_ratios[4] = 0.85f;
  d.final_drive = 3.4f;
  d.shift_up_rpm = 4600.0f;
  d.shift_down_rpm = 2400.0f;
  d.max_steer_angle = 0.55f;
  d.steer_high_speed_fraction = 0.45f;
  d.steer_fade_speed = 40.0f;
  d.anti_roll_front = 2600.0f;
  d.anti_roll_rear = 1600.0f;  // modest, softer rear
  d.front_suspension_frequency = 1.9f;
  d.rear_suspension_frequency = 1.6f;  // softer rear squats and hooks up
  d.suspension_damping = 0.42f;
  d.tire_long_friction = 0.7f;
  d.front_lat_friction = 1.25f;
  d.rear_lat_friction = 0.62f;  // rear well below front -> power oversteer
  d.brake_bias_front = 0.58f;
  d.max_brake_torque = 3000.0f;
  // No traction control: a raw muscle car lights up the rears.
  return d;
}

PhysicsWorld::VehicleDesc HatchbackProfile() {
  Desc d;
  d.half_extent = {0.86f, 0.60f, 1.92f};  // small, tallish, ~3.85 m
  d.mass = 1150.0f;
  d.wheel_radius = 0.30f;
  d.wheel_width = 0.20f;
  d.wheel_x = 0.76f;
  d.front_z = 1.26f;
  d.rear_z = -1.24f;
  d.suspension_min = 0.13f;
  d.suspension_max = 0.34f;
  d.com_drop = 0.34f;
  d.drivetrain = PhysicsWorld::Drivetrain::kFWD;
  d.limited_slip_ratio = 0;  // open diff, economy car
  d.max_engine_torque = 200.0f;  // economical
  // FWD-specific hard top-end taper: the front wheels unload and spin under
  // power, and at a gear's redline any residual torque keeps them slipping >
  // 10%, which permanently blocks Jolt's automatic upshift. Cutting near-redline
  // torque hard lets the box shift up cleanly. (RWD/AWD profiles don't need it.)
  d.torque_curve[0] = {0.10f, 0.60f};
  d.torque_curve[1] = {0.35f, 0.95f};
  d.torque_curve[2] = {0.55f, 1.00f};
  d.torque_curve[3] = {0.80f, 0.80f};
  d.torque_curve[4] = {1.00f, 0.38f};
  d.torque_curve_count = 5;
  // A slightly revvy little engine: with the hard FWD taper the box needs rpm
  // headroom above 100 km/h or it pins at a gear's top and can't upshift.
  d.max_rpm = 7000.0f;
  d.min_rpm = 950.0f;
  d.engine_inertia = 0.28f;
  d.gear_count = 4;
  d.gear_ratios[0] = 3.3f;
  d.gear_ratios[1] = 1.6f;  // a tall 2nd so 100 km/h sits mid-gear, not at the
                            // wheelspin-prone top where the FWD upshift stalls
  d.gear_ratios[2] = 1.15f;
  d.gear_ratios[3] = 0.82f;
  d.final_drive = 3.7f;
  d.shift_up_rpm = 4800.0f;
  d.shift_down_rpm = 2400.0f;
  d.max_steer_angle = 0.60f;
  d.steer_high_speed_fraction = 0.5f;
  d.steer_fade_speed = 38.0f;
  d.anti_roll_front = 1500.0f;
  d.anti_roll_rear = 1000.0f;
  d.suspension_frequency = 1.55f;  // soft-ish
  d.suspension_damping = 0.45f;
  d.tire_long_friction = 1.15f;
  d.front_lat_friction = 0.9f;   // front washes wide first -> understeer
  d.rear_lat_friction = 1.05f;
  d.brake_bias_front = 0.65f;
  d.max_brake_torque = 2200.0f;
  // traction control off: the tall 2nd gear keeps the front wheels below the
  // slip that would stall the auto upshift, and off-throttle it launches crisper.
  return d;
}

PhysicsWorld::VehicleDesc SuvProfile() {
  Desc d;
  d.half_extent = {0.98f, 0.78f, 2.30f};  // tall, ~4.6 m
  d.mass = 2100.0f;
  d.wheel_radius = 0.38f;
  d.wheel_width = 0.28f;
  d.wheel_x = 0.96f;
  d.front_z = 1.50f;
  d.rear_z = -1.40f;
  d.suspension_min = 0.20f;
  d.suspension_max = 0.55f;  // long travel
  d.com_drop = 0.50f;        // ballasted so it leans but stays on its wheels
  d.drivetrain = PhysicsWorld::Drivetrain::kAWD;
  d.awd_front_split = 0.3f;  // 30/70 front/rear: less front spin, still sure-footed
  d.limited_slip_ratio = 1.5f;
  d.max_engine_torque = 350.0f;
  // AWD: the front axle is driven too, so (like the FWD hatch) it wants a
  // moderate top-end taper - enough to keep pulling, but not so much that the
  // lightly-loaded front wheels spin at redline and stall the auto upshift.
  d.torque_curve[0] = {0.10f, 0.95f};
  d.torque_curve[1] = {0.28f, 1.00f};
  d.torque_curve[2] = {0.55f, 1.00f};
  d.torque_curve[3] = {0.80f, 0.86f};
  d.torque_curve[4] = {1.00f, 0.58f};
  d.torque_curve_count = 5;
  d.max_rpm = 6600.0f;
  d.min_rpm = 900.0f;
  d.engine_inertia = 0.5f;
  d.gear_count = 6;
  d.gear_ratios[0] = 3.6f;
  d.gear_ratios[1] = 2.3f;
  d.gear_ratios[2] = 1.6f;
  d.gear_ratios[3] = 1.2f;
  d.gear_ratios[4] = 0.95f;
  d.gear_ratios[5] = 0.78f;
  d.final_drive = 3.6f;
  d.shift_up_rpm = 4700.0f;
  d.shift_down_rpm = 2200.0f;
  d.max_steer_angle = 0.55f;
  d.steer_high_speed_fraction = 0.55f;
  d.steer_fade_speed = 35.0f;
  d.anti_roll_front = 2600.0f;
  d.anti_roll_rear = 2100.0f;  // moderate: leans some
  d.suspension_frequency = 1.6f;
  d.suspension_damping = 0.5f;
  // All-terrain tyres: a touch less peak tarmac grip, but the surface grip
  // table already favours AWD on dirt/grass; keep street grip near stock.
  d.tire_long_friction = 1.0f;
  d.tire_lat_friction = 0.9f;
  d.brake_bias_front = 0.6f;
  d.max_brake_torque = 3200.0f;
  // traction control off: launch differentiation comes from drivetrain + grip.
  return d;
}

PhysicsWorld::VehicleDesc VanProfile(f32 cargo_load) {
  const f32 load = cargo_load < 0 ? 0 : (cargo_load > 1 ? 1 : cargo_load);
  Desc d;
  d.half_extent = {0.95f, 0.98f, 2.60f};  // tall box, ~5.2 m, narrow-ish track
  d.mass = 2000.0f + 400.0f * load;       // empty ~2000, laden ~2400
  d.wheel_radius = 0.36f;
  d.wheel_width = 0.24f;
  d.wheel_x = 0.90f;  // a little narrow vs the tall body
  d.front_z = 1.70f;
  d.rear_z = -1.68f;
  d.suspension_min = 0.18f;
  d.suspension_max = 0.50f;
  d.com_drop = 0.55f - 0.16f * load;   // ballasted low; roof load raises the CG
  d.com_fore = -0.35f * load;          // cargo sits over/behind the rear axle
  d.drivetrain = PhysicsWorld::Drivetrain::kRWD;
  d.limited_slip_ratio = 2.0f;
  d.max_engine_torque = 430.0f;
  DieselCurve(d);
  // Revvier than a real diesel: the auto box needs rpm headroom above 100 km/h
  // in each gear, or the heavy laden van pins at a gear's top without upshifting.
  d.max_rpm = 5000.0f;
  d.min_rpm = 750.0f;
  d.engine_inertia = 0.7f;
  d.gear_count = 6;
  d.gear_ratios[0] = 3.6f;
  d.gear_ratios[1] = 2.2f;
  d.gear_ratios[2] = 1.4f;
  d.gear_ratios[3] = 0.98f;
  d.gear_ratios[4] = 0.78f;
  d.gear_ratios[5] = 0.65f;
  d.final_drive = 4.0f;
  d.shift_up_rpm = 3600.0f;
  d.shift_down_rpm = 1700.0f;
  d.max_steer_angle = 0.48f;  // slow steering
  d.steer_high_speed_fraction = 0.6f;
  d.steer_fade_speed = 30.0f;
  d.anti_roll_front = 1100.0f;
  d.anti_roll_rear = 900.0f;
  d.suspension_frequency = 1.3f;
  d.suspension_damping = 0.5f;
  d.tire_long_friction = 0.95f;
  d.tire_lat_friction = 0.85f;
  d.brake_bias_front = 0.62f;
  d.max_brake_torque = 3000.0f;
  // traction control off: launch differentiation comes from drivetrain + grip.
  return d;
}

PhysicsWorld::VehicleDesc SemiTruckProfile() {
  Desc d;
  d.half_extent = {1.25f, 1.40f, 3.00f};  // huge, tall cab, ~6 m tractor
  d.mass = 8500.0f;
  d.wheel_radius = 0.52f;
  d.wheel_width = 0.38f;
  d.wheel_x = 1.05f;
  d.front_z = 1.90f;
  d.rear_z = -1.90f;
  d.suspension_min = 0.22f;
  d.suspension_max = 0.55f;
  d.com_drop = 0.65f;  // ballasted low so the tall body leans hard but stays up
  d.drivetrain = PhysicsWorld::Drivetrain::kRWD;
  d.limited_slip_ratio = 2.5f;
  d.max_engine_torque = 3400.0f;  // enormous diesel torque
  DieselCurve(d);
  // Revvier than a real truck diesel: with this mass the auto box needs generous
  // rpm headroom per gear or it pins well short of 100 km/h. Top speed is still
  // capped by the tall gearing, and the 0-100 stays glacial (~1 min+).
  d.max_rpm = 4800.0f;
  d.min_rpm = 600.0f;
  d.engine_inertia = 1.4f;
  d.gear_count = 4;
  d.gear_ratios[0] = 5.0f;  // tall creeper first, then a wide box
  d.gear_ratios[1] = 2.8f;
  d.gear_ratios[2] = 1.6f;
  d.gear_ratios[3] = 1.0f;
  d.final_drive = 3.4f;
  d.shift_up_rpm = 3600.0f;
  d.shift_down_rpm = 1700.0f;
  d.max_steer_angle = 0.42f;  // very slow, big-truck steering
  d.steer_high_speed_fraction = 0.6f;
  d.steer_fade_speed = 25.0f;
  d.anti_roll_front = 700.0f;
  d.anti_roll_rear = 550.0f;  // soft: the tall body leans the most
  d.suspension_frequency = 1.2f;
  d.suspension_damping = 0.5f;
  d.tire_long_friction = 1.0f;
  d.tire_lat_friction = 0.85f;
  d.brake_bias_front = 0.5f;
  d.max_brake_torque = 5200.0f;  // weak per kg -> long stopping distances
  // traction control off: launch differentiation comes from drivetrain + grip.
  return d;
}

}  // namespace rx::physics
