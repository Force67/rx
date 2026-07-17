#ifndef RX_PHYSICS_KITE_H_
#define RX_PHYSICS_KITE_H_

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "physics/physics_world.h"

namespace rx::physics {

// Force-based tethered-kite simulator layered on the public PhysicsWorld
// rigid-body primitives (AddForceAtPoint / AddTorque / GetPointVelocity). One
// Kite owns one light dynamic sail body; the game moves an ANCHOR world point
// each frame (a hand, a post, a moving vehicle) and drives the kite with a
// two-axis input (steer + reel), reading telemetry back for HUD/camera.
//
// Model summary (all SI: metres, kg, seconds, newtons; +Z forward, +Y up,
// right-handed). Unlike the boat/aircraft this body is HEAVIER-THAN-AIR and
// gets ordinary Jolt gravity - the sail flies by aerodynamic lift, not
// buoyancy, so nothing is buoyancy-exempted and no gravity is added here.
//  * Sail frame: the sail lies in the body X-Y plane, spanwise along body X,
//    leading edge / nose toward body +Y, tail hanging toward body -Y, sail
//    NORMAL along body +Z. The visual/collision plate is span x height x a thin
//    thickness; the aerodynamic reference area (sail_area_m2) is separate (a
//    delta is roughly half its bounding box).
//  * Flat-plate aero via a NORMAL-FORCE decomposition (robust at every incidence,
//    which a kite needs: it trims at high alpha and must survive tumbling and
//    violent gusts without a lift/drag-direction singularity). The relative wind
//    w = ambient wind - the aero centre's point velocity is split into a normal
//    component (wn = w . n) and a tangential one. The pressure force is
//        F_n = 0.5 rho A cn (w . n) |w|  along n
//    so its effective coefficient CN = cn sin(alpha) grows with incidence (thin-
//    plate linear region); its VERTICAL part is lift and its downwind part is
//    drag - exactly how a sail generates both from one inclined pressure force.
//    A small tangential (skin/edge) drag and the tail add the rest. Equivalent
//    CL/CD: CL ~ cn sin(alpha) cos(alpha) = (cn/2) sin(2a) (peaks near 45 deg),
//    CD ~ cd0 + cn sin^2(alpha) (grows to a broadside parachute at 90 deg).
//  * A tail: a bluff drag patch on a long lever down body -Y. It weathervanes
//    the nose into the wind and damps yaw/roll/pitch oscillation (a real tail).
//  * Aero centre offset above the centre of mass gives pendulum stability; the
//    aero force applied AT the aero centre supplies the trimming moment.
//  * Tether: a stiff ONE-SIDED spring (a string only pulls, never pushes) from
//    the bridle point on the sail to the anchor, damped along the line, with the
//    rest length reeled within [min,max]. The tension is HARD-CAPPED at
//    tether_max_tension so a fast-moving anchor (towed kitesurf-style behind a
//    vehicle) cannot inject an explosive impulse - the documented blow-up guard.
//  * Two-line steering: steer models the stunt-kite line-warp as a moment about
//    the line-of-sight (anchor->kite) axis, banking the sail so its lift vector
//    carves a turn. It is scaled by dynamic pressure, so control authority
//    vanishes as the wind dies (the kite that stops answering the lines as it
//    falls) - emergent loops and dives under steer, a limp fall in dead air.
//
// UPDATE CONTRACT: call Kite::Update(input, dt) once per fixed step BEFORE
// PhysicsWorld::Update(dt), with set_anchor() already pointing at this step's
// anchor. Update() only accumulates forces/torques on the sail body (Jolt clears
// them after its own step) and refreshes telemetry from the pose sampled at the
// start of the step. dt must match the world's fixed step (~1/60 s).

// Per-frame control. Both -1..1.
struct KiteInput {
  f32 steer = 0.0f;  // -1 left .. +1 right (two-line warp, banks the sail)
  f32 reel = 0.0f;   // -1 reel IN (shorten) .. +1 reel OUT (lengthen) the line
};

// Kite definition. Defaults describe a ~1.5 m sport/delta kite; override for
// other sizes. See the force models in kite.cc for how each field is used.
struct RX_PHYSICS_EXPORT KiteDesc {
  // --- sail geometry (collision/visual plate) & mass ---
  f32 span_m = 1.5f;        // wingspan, body X
  f32 height_m = 1.0f;      // nose-to-tail height of the sail, body Y
  f32 thickness_m = 0.04f;  // plate thickness, body Z (collision + inertia only)
  f32 sail_area_m2 = 0.8f;  // aerodynamic reference area (< span*height for a delta)
  f32 mass_kg = 0.3f;       // sail + spars

  // --- flat-plate aero (normal-force model; see the header) ---
  f32 normal_coeff = 2.0f;      // cn: CN = cn sin(alpha), flat-plate normal slope
  f32 tangential_coeff = 0.12f;  // skin/edge drag along the sail surface
  // Aerodynamic centre (centre of pressure) in the body frame, CoM-relative.
  // Kept AT the centre of mass so the aero force adds no pitching moment that
  // would fight the attitude trim below (pitch/roll authority is owned by the
  // attitude alignment + tail; pendulum stability comes from the below-CoM
  // bridle). A small offset here is a knob for extra weathervaning if wanted.
  Vec3 aero_center{0.0f, 0.0f, 0.0f};

  // --- attitude trim (why a kite flies belly-to-wind at high, not zero, alpha) --
  // A kite's bridle geometrically holds the sail at a set angle to the lines, its
  // camber adds a pitching moment and its tail weathervanes it, so the sail flies
  // belly-INTO-the-wind at a fixed high angle of attack instead of feathering or
  // flipping. Modelled as a restoring torque that aligns the belly normal (body
  // +Z) with a target computed each step from the relative wind: the direction
  // that sits at trim_alpha_rad incidence with the belly UP (lift positive) - an
  // unambiguous, non-flippable target (a symmetric pitch-only trim has a second,
  // belly-down equilibrium the tether can knock it into). Torque =
  // attitude_stiffness * q_dyn * (n_current x n_target), so it fades with dynamic
  // pressure: firm in wind, gone in dead air (the kite goes limp and falls). It
  // leaves rotation ABOUT the normal free, which is what steering and the tail
  // act on.
  f32 trim_alpha_rad = 0.40f;      // ~23 deg, the incidence the bridle+camber sets
  f32 attitude_stiffness = 0.30f;  // belly-normal alignment gain (per unit q_dyn)

  // --- tail (yaw/roll/pitch damper + weathervane) ---
  f32 tail_length_m = 1.4f;  // tail patch hangs this far down body -Y from the CoM
  f32 tail_area_m2 = 0.05f;  // effective drag area of the tail
  f32 tail_drag = 1.2f;      // bluff drag coefficient of the tail

  // --- bridle + tether ---
  // Bridle attach point on the sail (body frame, CoM-relative). Kept CLOSE to the
  // CoM: the attitude trim owns stability, and a long bridle lever would let a
  // tension spike (a taut-line snap, a fast tow) torque the sail hard enough to
  // flip it. A small below-centre offset adds a touch of pendulum feel without
  // giving tension the leverage to overpower the trim.
  Vec3 bridle_point{0.0f, -0.08f, 0.02f};
  f32 min_line_m = 6.0f;        // shortest reelable line
  f32 max_line_m = 60.0f;       // longest reelable line
  f32 line_length_m = 25.0f;    // initial rest length (clamped into [min,max])
  // m/s the rest length is paid in/out at full reel input. Kept modest so the
  // kite can physically follow the shortening line; reeling far faster than the
  // sail can move would build huge spring stretch and yank it.
  f32 reel_rate = 2.5f;
  f32 tether_stiffness = 250.0f;  // N/m, stiff one-sided spring
  // Near-critical for the light sail (c ~ 2 sqrt(k m)), so the taut line does not
  // ring against the tiny kite mass at 60 Hz.
  f32 tether_damping = 20.0f;     // N per (m/s) of stretch rate along the line
  // Hard cap on tether tension, N. A one-sided spring against a fast-moving
  // anchor (towed behind a vehicle) would otherwise spike; clamping keeps the
  // kitesurf/tow case bounded and NaN-free. Sized well above steady flight
  // tension (a few tens of N) so it never clips normal flight.
  f32 tether_max_tension = 1500.0f;

  // --- two-line steering ---
  // Moment about the line-of-sight axis per unit dynamic pressure*area (so it
  // reads as an effective lever arm, m). Larger = twitchier stunt response.
  f32 steer_authority = 0.45f;

  // --- stability / robustness ---
  // Sized to (over)damp the attitude spring on the light sail so it settles onto
  // its trim without ringing, and to keep violent gusts bounded (the D term of
  // the attitude PD; the tail adds more).
  f32 angular_damping = 3.0f;     // Nm per (rad/s)
  // Whole-sail form-drag on the body's linear velocity, N per (m/s). Damps the
  // TANGENTIAL swing as the kite arcs from launch up to its equilibrium (the
  // tether only damps radial motion, so an undamped light sail slingshots past
  // and whips). Zero at a stationary equilibrium, so it costs nothing in steady
  // flight - it only bleeds the transient. Represents drag the sail model does
  // not otherwise capture (frame, bridle lines, edge vortices).
  f32 linear_damping = 1.4f;
  f32 min_airspeed_mps = 0.4f;    // below this the aero pass is skipped (1/|w| guard)

  // --- gusts (optional, default OFF so tests driving set_wind stay exact) ---
  f32 gust_amplitude_mps = 0.0f;  // 0 disables the internal gust generator
  Vec3 gust_dir{0.0f, 0.0f, 1.0f};  // gust blows along this (normalized) direction
};

// Telemetry snapshot, refreshed by Update. All plain data for HUD/camera.
struct KiteState {
  Vec3 position{};        // sail body centre, world
  Quat rotation{};        // sail orientation, world (x,y,z,w)
  f32 altitude_m = 0.0f;  // sail height above the anchor (position.y - anchor.y)
  f32 tension_n = 0.0f;   // current tether tension, N (0 when slack)
  bool taut = false;      // line taut this step
  f32 airspeed_mps = 0.0f;  // |relative wind| at the aero centre
  f32 alpha_deg = 0.0f;   // angle of attack (sail incidence to the relative wind)
  f32 line_length_m = 0.0f;  // current rest length (reel-adjustable)
};

class RX_PHYSICS_EXPORT Kite {
 public:
  // Spawns the sail body at `position` (sail centre) yawed `yaw_radians` about
  // +Y, with the tether anchored at `anchor`. Place the kite a little downwind
  // of the anchor (near the ground for a launch, or already aloft) and let the
  // wind fill it. `world` must outlive the Kite. valid() is false when the body
  // could not be created (physics stub).
  Kite(PhysicsWorld& world, const KiteDesc& desc, const Vec3& anchor, const Vec3& position,
       f32 yaw_radians);

  Kite(const Kite&) = delete;
  Kite& operator=(const Kite&) = delete;

  // Stages this step's aero + tail + steering + tether forces on the sail body.
  // Call once per fixed step, BEFORE PhysicsWorld::Update(dt). dt must be the
  // world's fixed step.
  void Update(const KiteInput& input, f32 dt);

  // Moves the tether anchor (hand / post / towing vehicle). Call every frame the
  // anchor moves; the one-sided spring pulls the kite toward wherever it is.
  void set_anchor(const Vec3& anchor) { anchor_ = anchor; }
  Vec3 anchor() const { return anchor_; }

  // Current reelable line length (rest length of the tether spring), metres.
  f32 line_length() const { return line_length_; }

  const KiteState& state() const { return state_; }
  const KiteDesc& desc() const { return desc_; }
  BodyId body() const { return body_; }
  bool valid() const { return body_ != 0; }

 private:
  PhysicsWorld& world_;
  KiteDesc desc_;
  BodyId body_ = 0;
  Vec3 anchor_{};
  f32 line_length_ = 0.0f;  // current tether rest length, m (reeled in [min,max])
  f32 gust_time_ = 0.0f;    // internal gust phase clock, s
  // Low-pass of the apparent wind used ONLY to aim the attitude-trim target, so
  // the sail's orientation tracks the mean airmass and does not chase its own
  // fast motion (which would flip it during a violent launch). The aero FORCE
  // still uses the instantaneous wind. Zero until the first step primes it.
  Vec3 wind_ref_{};
  bool wind_ref_primed_ = false;
  KiteState state_;
};

}  // namespace rx::physics

#endif  // RX_PHYSICS_KITE_H_
