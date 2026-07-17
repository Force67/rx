#ifndef RX_PHYSICS_BOAT_H_
#define RX_PHYSICS_BOAT_H_

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "physics/physics_world.h"

namespace rx::physics {

// Force-based motorboat simulator layered on the public PhysicsWorld rigid-body
// primitives (AddForceAtPoint / AddForce / AddTorque / SampleWater). One Boat
// owns one dynamic hull body; the game drives it with an input each frame and
// reads telemetry back for HUD/audio/camera.
//
// Model summary (all SI: metres, kg, seconds, newtons; +Z forward, +Y up,
// right-handed):
//  * Multi-point hull buoyancy over a grid on the hull bottom, applied per
//    sample so swell tilts the boat and the grid + a lowered centre of mass
//    self-right it from a knock-down.
//  * Quadratic hull drag (fore/aft asymmetric longitudinal + strong lateral
//    keel), scaled by the wetted fraction, all taken relative to the water flow
//    so rivers carry the hull.
//  * Speed-dependent planing: past hull speed the bow lifts and the wetted
//    longitudinal drag drops, so a planing hull tops out faster.
//  * Propeller thrust vs a spooling engine rpm, applied at a stern point ONLY
//    while that point is submerged (launch off a wave and the screw loses bite).
//  * A rudder sideforce at the stern from water speed + propeller wash, so the
//    boat still turns on the wash at a standstill.
//
// UPDATE CONTRACT: call Boat::Update(input, dt) once per fixed step BEFORE
// PhysicsWorld::Update(dt). Update() only accumulates forces on the hull body
// (Jolt clears them after its own step), so ordering matters: forces staged
// this frame are consumed by the very next PhysicsWorld::Update. dt must match
// the world's fixed step (~1/60 s).
struct BoatDesc {
  // Hull box half extents, boat-local: x = half beam (to +X), y = half height
  // (to +Y), z = half length (+Z forward). Default ~ a 6 m motorboat: 1.8 m
  // beam, 1.0 m tall hull, 5.8 m long.
  Vec3 hull_half_extent{0.9f, 0.5f, 2.9f};
  f32 mass = 1400.0f;  // kg (hull + engine + fuel)
  // Ballast keel: the effective centre of mass sits this many metres below the
  // geometric centre along boat-local -Y. Jolt keeps the real CoM at the box
  // centre, so this is emulated as a gravity righting couple each step
  // (torque = r_ballast x m*g); it is what self-rights the hull from a
  // knock-down. 0 disables the couple (grid buoyancy still rights, weakly).
  f32 com_drop = 0.35f;

  // Volumetric buoyancy sample grid filling the hull box: grid_beam across X,
  // grid_height up Y, grid_len along Z. Each sample owns an equal share of the
  // hull volume and displaces it while submerged, so the centre of buoyancy
  // shifts to the low side when the hull heels (righting) and toward the stern
  // when the bow lifts (pitch stability) - true metacentric behaviour, not the
  // bottom-face approximation. grid_height >= 2 is what makes the grid
  // self-right a knock-down; denser = smoother swell response. Cost is
  // grid_beam*grid_height*grid_len SampleWater calls per step.
  u32 grid_beam = 3;
  u32 grid_height = 3;
  u32 grid_len = 4;
  // Vertical heave/roll damping per submerged sample: newtons per (m/s) of the
  // sample's vertical velocity, scaled by its submerged fraction. Sized so the
  // default hull+grid settles heave without ringing at 60 Hz.
  f32 heave_damping = 1100.0f;

  // --- engine / propeller ---
  f32 max_thrust = 8000.0f;     // N delivered at max rpm, forward
  f32 idle_rpm = 800.0f;        // rpm at zero throttle (no thrust)
  f32 max_rpm = 5000.0f;        // rpm at full throttle
  f32 spool_time = 0.45f;       // s, first-order lag of rpm toward its target
  f32 reverse_fraction = 0.5f;  // astern thrust as a fraction of forward thrust
  // Propeller attach point, boat-local: stern, below the waterline. Thrust is
  // applied here and ONLY while this point is under the surface.
  Vec3 prop_offset{0.0f, -0.48f, -2.7f};

  // --- rudder ---
  // Stern sideforce = steer * (rudder_speed_gain * v_fwd^2
  //                            + rudder_wash_gain * |thrust|), applied at
  // rudder_offset. The wash term keeps steering authority at a standstill.
  f32 rudder_speed_gain = 9.0f;   // N per (m/s)^2 of water speed over the rudder
  f32 rudder_wash_gain = 0.06f;   // N of sideforce per N of propeller thrust
  Vec3 rudder_offset{0.0f, -0.45f, -2.8f};
  // Yaw-rate damping (Nm per rad/s). Water gives little yaw damping, but a
  // small amount makes turns settle to a steady rate and stops the boat
  // spinning when the helm is centred.
  f32 yaw_damping = 6000.0f;

  // --- hull drag (quadratic: N per (m/s)^2), scaled by the wetted fraction ---
  f32 drag_fwd = 110.0f;      // longitudinal, moving ahead (streamlined bow)
  f32 drag_aft = 260.0f;      // longitudinal, moving astern (blunt transom)
  f32 drag_lateral = 1400.0f; // keel resists sideslip (high -> the hull carves)
  f32 drag_vertical = 500.0f; // extra whole-hull heave drag (N per (m/s)^2)

  // --- planing ---
  f32 hull_speed = 5.0f;         // m/s where the bow starts to lift
  f32 plane_full_speed = 9.0f;   // m/s where the hull is fully planing
  f32 plane_lift = 90.0f;        // N per (m/s)^2 of dynamic bow lift when planing
  f32 plane_lift_cap = 1.0f;     // cap on the lift as a multiple of the weight
                                 // (before the wetted-fraction gate)
  f32 plane_drag_reduction = 0.6f;  // fraction the longitudinal drag drops at full plane

  // Bow-attitude trim from BoatInput::trim, as a pitch torque (Nm) at trim=1.
  // Positive trim pitches the bow up (e.g. trim tabs / outdrive out).
  f32 trim_torque = 3000.0f;

  // --- wind load on the exposed topsides ---
  // Drag coefficient of the above-water hull. The global PhysicsWorld::wind()
  // pushes on the exposed (above-water) topside area with a force quadratic in
  // the wind speed relative to the hull, scaled by the exposed fraction, and
  // applied above the waterline so a strong beam wind heels the boat slightly.
  // Air is ~1.225 kg/m^3 and the box topside areas are modest, so this is a
  // conservative nudge, not a capsizing force. 0 disables it.
  f32 wind_drag = 0.8f;
};

// Per-frame driver input. throttle and steer are -1..1; trim is -1..1 and
// optional (0 = neutral).
struct BoatInput {
  f32 throttle = 0.0f;  // -1 full astern .. +1 full ahead
  f32 steer = 0.0f;     // -1 .. +1 (rudder deflection; sign sets turn side)
  f32 trim = 0.0f;      // -1 bow down .. +1 bow up
};

// Telemetry for HUD, audio and camera. The audio module's VehicleAudioState
// maps trivially: rpm<-rpm, load<-engine_load, throttle<-throttle,
// speed<-speed_mps, submerged<-prop_submerged.
struct BoatState {
  f32 rpm = 0.0f;
  f32 engine_load = 0.0f;   // 0..1, delivered thrust fraction (spool-limited)
  f32 throttle = 0.0f;      // -1..1, last input throttle
  f32 speed_mps = 0.0f;     // world-space horizontal speed magnitude
  f32 forward_speed = 0.0f; // signed speed along the hull forward axis, m/s
  f32 planing = 0.0f;       // 0..1, planing fraction
  f32 wetted = 0.0f;        // 0..1, fraction of buoyancy samples submerged
  bool prop_submerged = false;
  Vec3 position{};          // hull body centre, world
  Quat rotation{};          // hull orientation, world (x,y,z,w)
};

class RX_PHYSICS_EXPORT Boat {
 public:
  // Spawns the hull body in `world` at `position` (hull centre) yawed around
  // +Y by `yaw_radians`. The body is exempted from the world's generic
  // buoyancy so only this model's hull forces act on it. `world` must outlive
  // the Boat. Check valid() for spawn success (false when physics is a stub).
  Boat(PhysicsWorld& world, const BoatDesc& desc, const Vec3& position, f32 yaw_radians);

  Boat(const Boat&) = delete;
  Boat& operator=(const Boat&) = delete;

  // Stages this frame's hull forces on the body. Call once per fixed step,
  // BEFORE PhysicsWorld::Update(dt). dt seconds must be the world's fixed step.
  void Update(const BoatInput& input, f32 dt);

  const BoatState& state() const { return state_; }
  BodyId body() const { return body_; }
  const BoatDesc& desc() const { return desc_; }
  bool valid() const { return body_ != 0; }

 private:
  PhysicsWorld& world_;
  BoatDesc desc_;
  BodyId body_ = 0;
  f32 rpm_ = 0.0f;  // engine speed, spooled toward the throttle target
  BoatState state_;
};

}  // namespace rx::physics

#endif  // RX_PHYSICS_BOAT_H_
