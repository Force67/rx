#include "physics/boat_profiles.h"

namespace rx::physics {

// Draft is emergent: at rest the hull settles to draft = mass / (rho * footprint)
// with footprint = (2*hex)*(2*hez), independent of hull height. The presets pick
// mass and footprint so the empty-draft ordering dinghy < speedboat < fishing <
// barge holds, then set hull height (hey) to the real hull side depth so the
// deck sits above the waterline with honest freeboard and an overload can bring
// it near awash. prop/rudder offsets ride at the stern just below the keel line.

BoatDesc DinghyProfile() {
  BoatDesc d;
  d.hull_half_extent = {0.62f, 0.30f, 1.50f};  // ~3 m, ~1.2 m beam
  d.mass = 220.0f;                             // hull + a small outboard
  d.com_drop = 0.10f;                          // shallow ballast -> tippy
  d.grid_beam = 3;
  d.grid_height = 3;
  d.grid_len = 3;
  d.heave_damping = 220.0f;
  d.max_thrust = 1300.0f;  // small outboard, but light -> planes early
  d.idle_rpm = 900.0f;
  d.max_rpm = 6000.0f;
  d.spool_time = 0.25f;
  d.reverse_fraction = 0.5f;
  d.prop_offset = {0.0f, -0.28f, -1.40f};
  d.rudder_speed_gain = 3.2f;
  d.rudder_wash_gain = 0.06f;
  d.rudder_offset = {0.0f, -0.26f, -1.45f};
  d.yaw_damping = 320.0f;  // twitchy
  d.drag_fwd = 40.0f;
  d.drag_aft = 90.0f;
  d.drag_lateral = 320.0f;
  d.drag_vertical = 150.0f;
  d.hull_speed = 2.5f;  // planes early
  d.plane_full_speed = 5.0f;
  d.plane_lift = 18.0f;
  d.plane_lift_cap = 0.6f;  // < weight so it planes without launching clear
  d.plane_drag_reduction = 0.6f;
  d.trim_torque = 400.0f;
  d.wind_drag = 1.0f;  // light hull, blown around
  d.max_cargo_kg = 200.0f;
  d.cargo_overload_fraction = 1.25f;
  d.cargo_com_offset = {0.0f, 0.28f, -0.05f};  // crew sits high -> quick to tip
  return d;
}

BoatDesc SpeedboatProfile() {
  // The BoatDesc defaults ARE the speedboat: ~6 m planing hull, 1400 kg, 8 kN
  // outdrive, com_drop 0.35. Kept as the defaults so BoatDesc{} reads as this
  // profile; listed explicitly here only for the handful of tuned knobs.
  BoatDesc d;
  d.max_cargo_kg = 1600.0f;  // a full load of passengers + gear buries the plane
  d.cargo_com_offset = {0.0f, 0.30f, -0.15f};
  return d;
}

BoatDesc JetskiProfile() {
  BoatDesc d;
  d.hull_half_extent = {0.50f, 0.35f, 1.10f};  // ~2.2 m
  d.mass = 350.0f;                             // ski + rider
  d.com_drop = 0.15f;                          // easily flipped, still self-rights
  d.grid_beam = 3;
  d.grid_height = 3;
  d.grid_len = 3;
  d.heave_damping = 1100.0f;  // keep the light hull from porpoising clear
  d.max_thrust = 2800.0f;    // extreme thrust-to-weight, still ~0.8 g
  d.idle_rpm = 1200.0f;
  d.max_rpm = 8000.0f;
  d.spool_time = 0.18f;
  d.reverse_fraction = 0.35f;
  d.prop_offset = {0.0f, -0.32f, -1.00f};
  d.rudder_speed_gain = 4.5f;
  d.rudder_wash_gain = 0.10f;
  d.rudder_offset = {0.0f, -0.30f, -1.05f};
  d.yaw_damping = 150.0f;  // spins on a dime
  d.drag_fwd = 35.0f;
  d.drag_aft = 90.0f;
  d.drag_lateral = 260.0f;
  d.drag_vertical = 500.0f;
  d.hull_speed = 1.5f;  // planes almost immediately
  d.plane_full_speed = 3.5f;
  d.plane_lift = 11.0f;
  d.plane_lift_cap = 0.45f;  // < weight so it planes without launching clear
  d.plane_drag_reduction = 0.65f;
  d.trim_torque = 300.0f;
  d.wind_drag = 0.6f;
  d.max_cargo_kg = 120.0f;  // a passenger + gear
  d.cargo_overload_fraction = 1.25f;
  d.cargo_com_offset = {0.0f, 0.22f, -0.05f};
  return d;
}

BoatDesc FishingBoatProfile() {
  BoatDesc d;
  d.hull_half_extent = {1.35f, 0.70f, 4.40f};  // ~9 m, ~2.7 m beam
  d.mass = 6500.0f;
  d.com_drop = 0.60f;  // deep ballast -> very stable
  d.grid_beam = 3;
  d.grid_height = 3;
  d.grid_len = 5;
  d.heave_damping = 6000.0f;
  d.max_thrust = 11000.0f;  // big diesel, but heavy -> slow
  d.idle_rpm = 600.0f;
  d.max_rpm = 3200.0f;
  d.spool_time = 0.9f;
  d.reverse_fraction = 0.55f;
  d.prop_offset = {0.0f, -0.62f, -4.20f};
  d.rudder_speed_gain = 48.0f;
  d.rudder_wash_gain = 0.05f;
  d.rudder_offset = {0.0f, -0.60f, -4.30f};
  d.yaw_damping = 60000.0f;  // slow, stable turns
  d.drag_fwd = 620.0f;
  d.drag_aft = 1200.0f;
  d.drag_lateral = 6000.0f;
  d.drag_vertical = 2500.0f;
  d.hull_speed = 6.5f;         // barely reaches...
  d.plane_full_speed = 13.0f;  // ...and never gets near full plane
  d.plane_lift = 15.0f;
  d.plane_lift_cap = 0.4f;
  d.plane_drag_reduction = 0.15f;
  d.trim_torque = 8000.0f;
  d.wind_drag = 1.2f;
  d.max_cargo_kg = 5000.0f;  // generous hold
  d.cargo_overload_fraction = 1.25f;
  d.cargo_com_offset = {0.0f, 0.32f, -0.20f};  // catch stacked high -> overload rights slowly
  return d;
}

BoatDesc WorkBargeProfile() {
  BoatDesc d;
  d.hull_half_extent = {1.80f, 0.70f, 5.50f};  // ~12 m, ~3.6 m beam, deep hold
  d.mass = 12000.0f;                           // heavy steel hull, deepest empty draft
  d.com_drop = 0.50f;
  d.grid_beam = 3;
  d.grid_height = 3;
  d.grid_len = 6;
  d.heave_damping = 12000.0f;
  d.max_thrust = 22000.0f;  // big torque, but enormous mass -> ponderous
  d.idle_rpm = 400.0f;
  d.max_rpm = 1800.0f;
  d.spool_time = 1.8f;  // slow-spooling
  d.reverse_fraction = 0.6f;
  d.prop_offset = {0.0f, -0.52f, -5.30f};
  d.rudder_speed_gain = 130.0f;
  d.rudder_wash_gain = 0.04f;
  d.rudder_offset = {0.0f, -0.50f, -5.40f};
  d.yaw_damping = 220000.0f;  // wide turning
  d.drag_fwd = 1400.0f;
  d.drag_aft = 2800.0f;
  d.drag_lateral = 14000.0f;
  d.drag_vertical = 6000.0f;
  d.hull_speed = 20.0f;         // never planes
  d.plane_full_speed = 40.0f;
  d.plane_lift = 0.0f;
  d.plane_lift_cap = 0.0f;
  d.plane_drag_reduction = 0.0f;
  d.trim_torque = 15000.0f;
  d.wind_drag = 1.5f;
  d.max_cargo_kg = 31000.0f;  // the showcase: tens of tonnes
  d.cargo_overload_fraction = 1.25f;
  d.cargo_com_offset = {0.0f, 0.15f, -0.20f};
  return d;
}

}  // namespace rx::physics
