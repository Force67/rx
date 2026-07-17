#include "physics/kite.h"

#include <algorithm>
#include <cmath>

namespace rx::physics {

namespace {

constexpr f32 kAirDensity = 1.225f;  // kg/m^3, sea-level ISA
constexpr f32 kRadToDeg = 57.2957795131f;

f32 Clamp(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Yaw quaternion about +Y (x,y,z,w), for spawn orientation.
Quat YawQuat(f32 yaw_radians) {
  return {0.0f, std::sin(yaw_radians * 0.5f), 0.0f, std::cos(yaw_radians * 0.5f)};
}

// Guards a force/torque against NaN/Inf before it reaches the solver.
bool Finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

Kite::Kite(PhysicsWorld& world, const KiteDesc& desc, const Vec3& anchor, const Vec3& position,
           f32 yaw_radians)
    : world_(world), desc_(desc), anchor_(anchor) {
  // Sail collision/visual plate: span x height x a thin thickness. Mass comes
  // from the box density (density = mass / volume) so Jolt derives exactly
  // desc.mass_kg; the aero model supplies all flight forces, gravity stays
  // Jolt's (the kite is heavier-than-air, so no buoyancy exemption).
  const Vec3 he{std::max(desc_.span_m, 0.05f) * 0.5f, std::max(desc_.height_m, 0.05f) * 0.5f,
                std::max(desc_.thickness_m, 0.01f) * 0.5f};
  const f32 volume = std::max(8.0f * he.x * he.y * he.z, 1e-4f);
  const f32 density = std::max(desc_.mass_kg, 1e-3f) / volume;
  // Spawn in a launch-ready attitude, not flat: a flat plate cannot catch the
  // wind (you prop a real kite up to launch it). Pitch the sail back about its
  // span so its belly presents to a tailwind at roughly the trim incidence, then
  // yaw. So the kite should be spawned FACING DOWNWIND (its local +Z toward where
  // the wind is going); the wind then fills it and it climbs off the ground.
  constexpr f32 kHalfPi = 1.57079632679f;
  const f32 launch_pitch = -(kHalfPi - desc_.trim_alpha_rad);  // belly to a +Z wind
  const Quat q = YawQuat(yaw_radians) * QuatFromAxisAngle(Vec3{1, 0, 0}, launch_pitch);
  const f32 rot[4] = {q.x, q.y, q.z, q.w};
  body_ = world_.AddDynamicBox(position, he, density, Vec3{});
  if (body_ == 0) return;  // stub / spawn failure
  // AddDynamicBox spawns axis-aligned; orient the sail to the spawn yaw.
  world_.SetBodyPosition(body_, position, rot);
  // Override the paper-thin plate's near-zero in-plane inertia with a modest,
  // honest tensor: a bare 0.04 m plate has ~0.03 kg m^2 about its span, so tiny
  // that any aero/attitude torque snaps it at 60 Hz. The real sail carries an
  // "added air mass" (the air it swings) that a rigid plate ignores; this floors
  // the tensor to that effective value so the attitude PD is stable. Scales with
  // the sail so bigger kites turn more slowly.
  const f32 s = std::max(desc_.span_m, 0.1f);
  const f32 h = std::max(desc_.height_m, 0.1f);
  const f32 inertia = std::max(desc_.mass_kg, 1e-3f) * 0.5f * (s * s + h * h) / 12.0f + 0.08f;
  world_.SetBodyInertia(body_, Vec3{inertia, inertia, inertia});
  line_length_ = Clamp(desc_.line_length_m, desc_.min_line_m, desc_.max_line_m);
  state_.position = position;
  state_.rotation = q;
  state_.line_length_m = line_length_;
}

Kite::~Kite() {
  if (body_ != 0) world_.RemoveBody(body_);
}

void Kite::Update(const KiteInput& input, f32 dt) {
  if (body_ == 0 || dt <= 0.0f) return;

  const f32 steer = Clamp(input.steer, -1.0f, 1.0f);
  const f32 reel = Clamp(input.reel, -1.0f, 1.0f);

  // --- reel: pay the tether rest length in/out within [min,max] ---
  line_length_ = Clamp(line_length_ + reel * desc_.reel_rate * dt, desc_.min_line_m,
                       desc_.max_line_m);

  // --- pose + body axes ---
  Vec3 pos{};
  f32 rot[4] = {0, 0, 0, 1};
  world_.GetBodyTransform(body_, &pos, rot);
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  const Vec3 sail_normal = Rotate(q, Vec3{0, 0, 1});  // body +Z

  // --- ambient airmass: world wind + optional internal gust ---
  gust_time_ += dt;
  Vec3 ambient = world_.wind();
  if (desc_.gust_amplitude_mps > 0.0f) {
    // Two-octave sinusoid: a cheap, bounded, deterministic gust. Along gust_dir.
    const f32 g = 0.6f * std::sin(gust_time_ * 0.7f) + 0.4f * std::sin(gust_time_ * 2.3f + 1.7f);
    ambient += Normalize(desc_.gust_dir) * (desc_.gust_amplitude_mps * g);
  }

  // --- flat-plate aero at the aero centre (normal-force decomposition) ---
  // Relative wind is taken at the aero centre's point velocity so the sail's own
  // rotation contributes (real damping). The pressure force acts along the sail
  // normal with magnitude 0.5 rho A cn (w . n) |w|; a small tangential drag adds
  // skin/edge losses. Both applied AT the aero centre so the CoP-CoM offset
  // trims the kite. Skipped below min_airspeed to guard the 1/|w| singularity.
  const Vec3 ac_world = pos + Rotate(q, desc_.aero_center);
  const Vec3 ac_vel = world_.GetPointVelocity(body_, ac_world);
  const Vec3 w = ambient - ac_vel;
  const f32 speed = Length(w);
  f32 alpha_deg = 0.0f;
  if (speed > desc_.min_airspeed_mps) {
    const f32 wn = Dot(w, sail_normal);        // signed normal component
    const Vec3 w_tan = w - sail_normal * wn;   // tangential component
    const f32 q_area = 0.5f * kAirDensity * std::max(desc_.sail_area_m2, 1e-3f);
    // Pressure normal force: F = q_area * cn * (w.n) * |w|, along the normal.
    // Effective CN = cn * sin(alpha), the flat-plate linear region.
    const Vec3 f_normal = sail_normal * (q_area * desc_.normal_coeff * wn * speed);
    // Tangential skin/edge drag along the sail surface: F = q_area * ct * |w| * w_tan.
    const Vec3 f_tan = w_tan * (q_area * desc_.tangential_coeff * speed);
    const Vec3 f_aero = f_normal + f_tan;
    if (Finite(f_aero)) world_.AddForceAtPoint(body_, f_aero, ac_world);
    // Angle of attack (sail incidence to the wind): sin(alpha) = (w.n)/|w|.
    const f32 alpha = std::asin(Clamp(wn / speed, -1.0f, 1.0f));
    alpha_deg = alpha * kRadToDeg;

    // --- attitude trim: align the belly normal to a belly-UP target ----------
    // Target normal = w_hat sin(trim) + up_perp cos(trim), where up_perp is world
    // up projected off the wind: this sits the sail at trim incidence with lift
    // positive (belly up), an unambiguous target that cannot flip belly-down. A
    // torque proportional to n x n_target, scaled by dynamic pressure, rotates
    // the sail onto it (firm in wind, gone in calm), representing the combined
    // bridle + camber + tail pitch/roll stabilisation. The target is aimed by a
    // LOW-PASS of the apparent wind (wind_ref_), so the sail tracks the mean
    // airmass and does not chase its own fast motion (which would flip it during
    // a violent launch); the aero force above stays instantaneous.
    if (!wind_ref_primed_) {
      wind_ref_ = w;
      wind_ref_primed_ = true;
    } else {
      constexpr f32 kAttTau = 0.35f;  // s, attitude-target smoothing time constant
      wind_ref_ += (w - wind_ref_) * (1.0f - std::exp(-dt / kAttTau));
    }
    const f32 ref_speed = Length(wind_ref_);
    if (desc_.attitude_stiffness > 0.0f && ref_speed > desc_.min_airspeed_mps) {
      const Vec3 w_hat = wind_ref_ * (1.0f / ref_speed);
      const Vec3 up{0, 1, 0};
      Vec3 up_perp = up - w_hat * Dot(up, w_hat);
      const f32 upl = Length(up_perp);
      // Wind near-vertical: fall back to the current normal's off-wind part so the
      // target stays well-defined (no divide-by-zero, no snap).
      up_perp = upl > 1e-3f ? up_perp * (1.0f / upl)
                            : Normalize(sail_normal - w_hat * Dot(sail_normal, w_hat));
      const f32 st = std::sin(desc_.trim_alpha_rad);
      const f32 ct = std::cos(desc_.trim_alpha_rad);
      const Vec3 n_target = w_hat * st + up_perp * ct;
      const f32 q_dyn = q_area * speed * speed;  // 0.5 rho A |w|^2
      const Vec3 align_torque = Cross(sail_normal, n_target) * (desc_.attitude_stiffness * q_dyn);
      if (Finite(align_torque)) world_.AddTorque(body_, align_torque);
    }
  }

  // --- tail: bluff drag on a long lever down body -Y ---
  // Weathervanes the nose into the wind and damps yaw/roll/pitch. Full quadratic
  // drag at the tail point, taken relative to the ambient air at that point.
  if (desc_.tail_area_m2 > 0.0f && desc_.tail_drag > 0.0f) {
    const Vec3 tail_world = pos + Rotate(q, Vec3{0, -desc_.tail_length_m, 0});
    const Vec3 tail_vel = world_.GetPointVelocity(body_, tail_world);
    const Vec3 w_tail = ambient - tail_vel;
    const f32 tail_speed = Length(w_tail);
    if (tail_speed > 1e-3f) {
      const Vec3 f_tail =
          w_tail * (0.5f * kAirDensity * desc_.tail_area_m2 * desc_.tail_drag * tail_speed);
      if (Finite(f_tail)) world_.AddForceAtPoint(body_, f_tail, tail_world);
    }
  }

  // --- two-line steering: a moment about the line-of-sight (anchor->kite) axis ---
  // Banks the sail so its lift vector carves a turn. Scaled by dynamic pressure
  // over the sail, so authority vanishes as the wind dies (a falling kite stops
  // answering the lines). Emergent loops/dives; limp in dead air.
  if (steer != 0.0f && speed > desc_.min_airspeed_mps) {
    const Vec3 los = pos - anchor_;
    const f32 losd = Length(los);
    if (losd > 1e-3f) {
      const Vec3 los_dir = los * (1.0f / losd);
      const f32 dyn = 0.5f * kAirDensity * std::max(desc_.sail_area_m2, 1e-3f) * speed * speed;
      const Vec3 steer_torque = los_dir * (steer * desc_.steer_authority * dyn);
      if (Finite(steer_torque)) world_.AddTorque(body_, steer_torque);
    }
  }

  // --- tether: stiff ONE-SIDED spring from the bridle to the anchor ---
  // A string only pulls: force applies only while the bridle is farther than the
  // rest length. Damped along the line. HARD-CAPPED at tether_max_tension so a
  // fast-moving anchor (towed behind a vehicle) cannot spike the solver.
  const Vec3 bridle_world = pos + Rotate(q, desc_.bridle_point);
  const Vec3 d = bridle_world - anchor_;
  const f32 dist = Length(d);
  bool taut = false;
  f32 tension = 0.0f;
  if (dist > line_length_ && dist > 1e-4f) {
    taut = true;
    const Vec3 dir = d * (1.0f / dist);  // anchor -> bridle
    const f32 stretch = dist - line_length_;
    const Vec3 bridle_vel = world_.GetPointVelocity(body_, bridle_world);
    const f32 rate = Dot(bridle_vel, dir);  // outward (stretching) speed
    f32 f = desc_.tether_stiffness * stretch + desc_.tether_damping * rate;
    f = Clamp(f, 0.0f, desc_.tether_max_tension);  // one-sided + blow-up guard
    tension = f;
    const Vec3 f_tether = dir * (-f);  // pull the kite toward the anchor
    if (Finite(f_tether)) world_.AddForceAtPoint(body_, f_tether, bridle_world);
  }

  // --- angular damping: the D term of the attitude PD + gust net ---
  if (desc_.angular_damping > 0.0f) {
    const Vec3 ang = world_.GetAngularVelocity(body_);
    const Vec3 damp = ang * (-desc_.angular_damping);
    if (Finite(damp)) world_.AddTorque(body_, damp);
  }

  // --- linear (whole-sail form-drag) damping: bleeds the tangential swing ---
  if (desc_.linear_damping > 0.0f) {
    const Vec3 vel = world_.GetLinearVelocity(body_);
    const Vec3 lin = vel * (-desc_.linear_damping);
    if (Finite(lin)) world_.AddForce(body_, lin);
  }

  // --- telemetry ---
  state_.position = pos;
  state_.rotation = q;
  state_.altitude_m = pos.y - anchor_.y;
  state_.tension_n = tension;
  state_.taut = taut;
  state_.airspeed_mps = speed;
  state_.alpha_deg = alpha_deg;
  state_.line_length_m = line_length_;
}

}  // namespace rx::physics
