#ifndef RX_PHYSICS_AIRCRAFT_H_
#define RX_PHYSICS_AIRCRAFT_H_

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "physics/physics_world.h"

namespace rx::physics {

// Force-based fixed-wing aircraft simulator. One instance drives one plane: it
// owns a single dynamic fuselage body in the PhysicsWorld and, each step,
// integrates a strip-theory aero model (wing halves, tail, fin), a propeller or
// jet, and a three-wheel landing gear into that body via the public force
// primitives (AddForce / AddForceAtPoint / AddTorque). Gravity, integration and
// contact response stay in Jolt; the simulator only supplies aero, thrust and
// suspension forces.
//
// Frame: engine convention, +Z forward, +Y up, right-handed, so the RIGHT wing
// is along -X and the LEFT wing along +X (right = forward x up = +Z x +Y = -X).
// Units are SI throughout (m, kg, s, N, Nm, rad). Air density is a constant
// 1.225 kg/m^3 (sea level, ISA); there is no wind field yet, only a settable
// uniform wind velocity hook that defaults to zero.
//
// Update ordering contract: call Update(input, dt) for every aircraft BEFORE
// PhysicsWorld::Update(dt) each fixed step, with the same dt. Update only
// accumulates forces on the body (Jolt clears them after it integrates), reads
// back nothing it just wrote, and refreshes the telemetry snapshot from the
// body pose sampled at the start of the step.

// Per-step pilot input. Elevator/aileron/rudder are normalized command axes,
// not deflection angles; the model maps them to control-surface ΔCL internally.
struct AircraftInput {
  f32 throttle = 0;  // 0..1, engine power / thrust demand
  f32 pitch = 0;     // -1..1 elevator, + = nose up (stick back)
  f32 roll = 0;      // -1..1 ailerons, + = roll right (right wing down)
  f32 yaw = 0;       // -1..1 rudder in the air, nose-wheel steer on the ground,
                     //   + = nose right
  f32 flaps = 0;     // 0..1 commanded flap fraction (quantized to flap_steps)
  f32 brakes = 0;    // 0..1 wheel brakes on the main gear
};

// Airframe definition. Defaults describe a Cessna-172-class light single;
// override for other types. Aerodynamic coefficients are per-radian unless
// noted. See the force models in aircraft.cc for how each field is used.
struct RX_PHYSICS_EXPORT AircraftDesc {
  enum class Propulsion : u8 { kProp, kJet };

  // One landing-gear leg: a downward suspension raycast from `local_pos` (a
  // CoM-relative body-frame attach point) plus a wheel of `radius`. The strut
  // extends `travel` metres; the spring/damper act along the contact normal.
  struct Wheel {
    Vec3 local_pos{};              // attach point, body frame (CoM-relative)
    f32 radius = 0.35f;            // wheel radius, m
    f32 travel = 0.50f;            // suspension stroke, m
    f32 spring = 40000.0f;         // N/m
    f32 damper = 6000.0f;          // N per m/s
    f32 brake_force = 3200.0f;     // max braking force, N (only if `braked`)
    f32 rolling_resistance = 0.03f;  // fraction of normal load opposing roll
    f32 lateral_grip = 1.1f;       // lateral friction coefficient (mu) of the
                                   //   tire; side force is capped at mu * load
    bool steerable = false;        // yaw input steers this wheel (nose gear)
    bool braked = false;           // wheel brakes act here (main gear)
  };

  // --- mass & structure ---
  // Collision box wraps the fuselage core only (not the wings/empennage) so it
  // clears the runway on its gear; the visual/aero model is larger.
  Vec3 fuselage_half_extent{0.55f, 0.6f, 2.6f};
  f32 empty_mass_kg = 767.0f;
  f32 max_takeoff_mass_kg = 1111.0f;  // MTOM (certified limit)
  f32 payload_kg = 220.0f;            // crew + fuel + cargo, fixed at creation
  // Hard structural cap, slightly above MTOM: payload is clamped so
  // empty + payload <= this. A plane loaded between MTOM and this limit is
  // created and flies, but it flies "like a pig": long ground roll, weak or no
  // climb (see the induced-drag / weight coupling in the model). over_mtom()
  // reports the between-MTOM-and-limit state.
  f32 structural_mass_limit_kg = 1300.0f;

  // --- wing (whole wing; the model splits it into two equal halves) ---
  f32 wing_area_m2 = 16.2f;
  f32 wing_span_m = 11.0f;
  f32 wing_zero_lift_alpha_rad = -0.035f;  // cambered section, ~ -2 deg
  f32 wing_cl_alpha = 5.1f;                // lift-curve slope, /rad (finite AR)
  f32 wing_stall_alpha_rad = 0.29f;        // ~16.5 deg
  f32 post_stall_decay = 0.12f;            // half-width (rad) of the stall
                                           //   blend into the flat-plate curve;
                                           //   larger softens the CL drop
  f32 oswald_efficiency = 0.75f;           // e in induced drag CL^2/(pi AR e)
  f32 cd0 = 0.028f;                        // parasitic drag coefficient

  // --- flaps ---
  f32 flap_delta_cl = 0.65f;   // ΔCL added at full flap deflection
  f32 flap_delta_cd = 0.06f;   // ΔCD added at full flap deflection
  u32 flap_steps = 3;          // detents between 0 and 1 (0/1/3 -> 0,.33,.66,1)

  // --- horizontal tail (elevator) ---
  f32 tail_area_m2 = 2.0f;
  f32 tail_arm_m = 4.6f;        // aerodynamic centre aft of CoM, m
  f32 tail_cl_alpha = 3.6f;     // tailplane lift-curve slope, /rad
  f32 elevator_authority = 1.1f;  // ΔCL_tail at full elevator

  // --- vertical fin (rudder) ---
  f32 fin_area_m2 = 1.1f;
  f32 fin_arm_m = 4.6f;         // aft of CoM, m
  f32 fin_cl_beta = 3.0f;       // side-force slope vs sideslip, /rad
  f32 rudder_authority = 0.9f;  // ΔCL_fin at full rudder

  // --- ailerons ---
  f32 aileron_authority = 0.5f;  // ΔCL differential between the wing halves at
                                 //   full roll input (outboard camber change)

  // --- fuselage side drag (sideslip / weathervane damping) ---
  f32 fuselage_side_cd = 0.55f;
  f32 fuselage_side_area_m2 = 6.5f;

  // --- rotational aerodynamic damping (per unit dynamic pressure) ---
  // Real airframes damp their own rotation strongly; without wings in the
  // collision box Jolt's inertia is small, so these keep roll/pitch/yaw rates
  // physical and post-stall tumbling bounded. Nm per (rad/s), scaled by dynamic
  // pressure relative to a 35 m/s reference.
  f32 roll_damp = 500.0f;
  f32 pitch_damp = 1800.0f;
  f32 yaw_damp = 1400.0f;

  // --- propulsion ---
  Propulsion propulsion = Propulsion::kProp;
  // Prop: momentum-theory-style thrust that falls off with airspeed,
  // T ~= min(power * eff / max(V, v_min), static_cap). rpm tracks throttle
  // through a first-order spool lag and is reported as telemetry.
  f32 prop_max_power_w = 134000.0f;   // ~180 hp
  f32 prop_diameter_m = 1.9f;
  f32 prop_max_rpm = 2700.0f;
  f32 prop_idle_rpm = 420.0f;
  f32 prop_efficiency = 0.8f;
  f32 prop_static_thrust_cap_n = 2600.0f;
  f32 prop_min_airspeed_mps = 12.0f;  // guards the 1/V static singularity
  f32 engine_spool_time_s = 0.7f;     // prop rpm first-order time constant
  // Jet: static thrust scaled by throttle through a spool lag; telemetry rpm
  // is reported as N1 percent (0..100).
  f32 jet_max_thrust_n = 24000.0f;
  f32 jet_spool_time_s = 3.5f;

  // --- landing gear: 0 = nose (steerable), 1 = left main, 2 = right main ---
  Wheel wheels[3];
  f32 nose_steer_angle_rad = 0.55f;  // full nose-wheel deflection (low speed)

  // Fills the Cessna-172-class defaults above, including the three wheels.
  AircraftDesc();
};

// Telemetry snapshot, refreshed by Update. Audio maps rpm/engine_load/throttle/
// airspeed directly (prop rpm for prop planes, N1 % for jets).
struct AircraftState {
  f32 airspeed_mps = 0;
  f32 vertical_speed_mps = 0;   // world +Y velocity
  f32 alpha_deg = 0;            // angle of attack
  f32 beta_deg = 0;            // sideslip
  bool stalled_left = false;    // left wing half past stall
  bool stalled_right = false;   // right wing half past stall
  f32 rpm = 0;                  // prop rpm, or N1 % (0..100) for jets
  f32 engine_load = 0;          // 0..1, delivered thrust vs available
  f32 throttle = 0;             // 0..1, filtered/commanded
  bool on_ground = false;       // any wheel in contact this step
  f32 gear_compression[3] = {}; // per wheel, 0 = extended, 1 = bottomed
  f32 total_mass_kg = 0;
  bool over_mtom = false;       // loaded above MTOM (flies like a pig)
  Vec3 position{};              // fuselage CoM, world
  Quat rotation{};              // fuselage orientation, world
};

class RX_PHYSICS_EXPORT Aircraft {
 public:
  // Spawns the fuselage body at `position` (CoM) yawed `yaw_radians` about +Y.
  // Spawn slightly above the runway and let the gear settle (a few steps of
  // zero input) before flying. The plane keeps a reference to `world`; it must
  // outlive the Aircraft. valid() is false if the body could not be created
  // (e.g. against the physics stub).
  Aircraft(PhysicsWorld& world, const AircraftDesc& desc, const Vec3& position, f32 yaw_radians);

  // Accumulates one fixed step of aero + thrust + gear forces on the body and
  // refreshes state(). Call once per step, BEFORE PhysicsWorld::Update(dt).
  void Update(const AircraftInput& input, f32 dt);

  const AircraftState& state() const { return state_; }
  const AircraftDesc& desc() const { return desc_; }
  BodyId body() const { return body_; }
  bool valid() const { return body_ != 0; }

  f32 total_mass() const { return total_mass_; }
  bool over_mtom() const { return total_mass_ > desc_.max_takeoff_mass_kg; }

  // Uniform wind (world m/s) added to the airmass; the one-line hook for a
  // future wind field. Defaults to zero (still air).
  void set_wind(const Vec3& wind) { wind_ = wind; }
  Vec3 wind() const { return wind_; }

 private:
  PhysicsWorld& world_;
  AircraftDesc desc_;
  BodyId body_ = 0;
  AircraftState state_;
  Vec3 wind_{};

  f32 total_mass_ = 0;   // empty + clamped payload
  f32 engine_spin_ = 0;  // 0..1 filtered engine state (rpm/N1 fraction)
  f32 flaps_ = 0;        // 0..1 filtered flap deflection
};

}  // namespace rx::physics

#endif  // RX_PHYSICS_AIRCRAFT_H_
