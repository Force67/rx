#include "physics/aircraft.h"

#include <algorithm>
#include <cmath>

#include "physics/shape_desc.h"

namespace rx::physics {

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kAirDensity = 1.225f;   // kg/m^3, sea-level ISA
constexpr f32 kRad2Deg = 57.2957795f;
// Reference dynamic pressure (0.5 * rho * 35^2) the rotational damping scales
// against, so damping tracks airspeed like a real stability derivative.
constexpr f32 kQRef = 0.5f * kAirDensity * 35.0f * 35.0f;

f32 Clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Smooth 0..1 ramp over [edge0, edge1].
f32 SmoothStep(f32 edge0, f32 edge1, f32 x) {
  if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
  f32 t = Clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Fraction (0 attached .. 1 fully stalled) of the flow at angle of attack
// `alpha`. A logistic centred on the stall angle blends the attached linear
// lift curve into a flat-plate curve; `half_width` (rad) sets how abruptly.
f32 StallBlend(f32 alpha, f32 stall_alpha, f32 half_width) {
  f32 x = (std::fabs(alpha) - stall_alpha) / std::max(half_width, 1e-3f);
  return 1.0f / (1.0f + std::exp(-4.0f * x));
}

}  // namespace

AircraftDesc::AircraftDesc() {
  // Nose gear (steerable) forward under the firewall; mains just aft of the CoM
  // and out on the track so the plane rests nose-light and rotates about the
  // mains. y is the fuselage-bottom attach height (CoM-relative). Right = -X.
  // Attach points sit just BELOW the fuselage collision box (box half-height
  // 0.6), because the public Raycast has no ignore-self filter: a strut-top ray
  // started inside the box would hit the plane's own underside. The nose leg is
  // on the centreline (inside the box footprint), so this matters for it.
  wheels[0].local_pos = Vec3{0.0f, -0.7f, 1.2f};   // nose
  wheels[0].steerable = true;
  wheels[0].braked = false;
  wheels[0].spring = 34000.0f;

  wheels[1].local_pos = Vec3{1.1f, -0.7f, -0.5f};  // left main (+X)
  wheels[1].braked = true;

  wheels[2].local_pos = Vec3{-1.1f, -0.7f, -0.5f};  // right main (-X)
  wheels[2].braked = true;
}

Aircraft::Aircraft(PhysicsWorld& world, const AircraftDesc& desc, const Vec3& position,
                   f32 yaw_radians)
    : world_(world), desc_(desc) {
  // Clamp payload to the hard structural limit; the plane still spawns when
  // loaded between MTOM and the limit, it just flies badly (see over_mtom()).
  const f32 max_payload = std::max(0.0f, desc_.structural_mass_limit_kg - desc_.empty_mass_kg);
  const f32 payload = Clampf(desc_.payload_kg, 0.0f, max_payload);
  desc_.payload_kg = payload;
  total_mass_ = desc_.empty_mass_kg + payload;

  // Fuselage collision box, spawned yawed about +Y. The box carries only the
  // fuselage (not the wings), so Jolt's derived inertia is small; the aero
  // model supplies its own rotational damping to compensate.
  ShapeDesc box;
  box.kind = ShapeDesc::Kind::kBox;
  box.half_extents = desc_.fuselage_half_extent;

  const Quat q = QuatFromAxisAngle(Vec3{0, 1, 0}, yaw_radians);
  const f32 rot[4] = {q.x, q.y, q.z, q.w};
  body_ = world_.AddDynamicShape(box, position, rot, 1.0f, total_mass_, 0.4f, 0.0f);

  // Prime telemetry so a read before the first Update is sane.
  state_.total_mass_kg = total_mass_;
  state_.over_mtom = total_mass_ > desc_.max_takeoff_mass_kg;
  state_.position = position;
  state_.rotation = q;
}

void Aircraft::Update(const AircraftInput& input, f32 dt) {
  if (body_ == 0 || dt <= 0.0f) return;

  // --- pose and body axes (sampled at the start of the step) ---
  Vec3 pos{};
  f32 rot[4] = {0, 0, 0, 1};
  world_.GetBodyTransform(body_, &pos, rot);
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  const Vec3 fwd = Rotate(q, Vec3{0, 0, 1});    // +Z forward
  const Vec3 up = Rotate(q, Vec3{0, 1, 0});     // +Y up
  const Vec3 left = Rotate(q, Vec3{1, 0, 0});   // +X = left wing
  const Vec3 right = left * -1.0f;              // -X = right wing

  const Vec3 vel = world_.GetLinearVelocity(body_);
  const Vec3 omega = world_.GetAngularVelocity(body_);
  const Vec3 vair_com = vel - wind_;
  const f32 speed = Length(vair_com);

  // Blend all aerodynamics out below ~1 m/s so the direction singularities
  // (lift axis, atan2 of a zero vector) never produce NaNs at rest.
  const f32 aero_fade = SmoothStep(1.0f, 3.0f, speed);

  // Filter flap deflection toward the quantized command (mechanical lag).
  {
    const u32 steps = std::max(1u, desc_.flap_steps);
    const f32 quantized = std::round(Clampf(input.flaps, 0.0f, 1.0f) * steps) / steps;
    const f32 rate = 0.5f;  // full travel in ~2 s
    flaps_ += Clampf(quantized - flaps_, -rate * dt, rate * dt);
  }

  const f32 q_com = 0.5f * kAirDensity * speed * speed;

  // Angle of attack / sideslip at the CoM (telemetry + fuselage drag). Guarded
  // by aero_fade so the atan2 of a near-zero velocity does not matter.
  const f32 f_com = Dot(vair_com, fwd);
  const f32 u_com = Dot(vair_com, up);
  const f32 s_com = Dot(vair_com, right);
  const f32 alpha_com = std::atan2(-u_com, f_com);
  const f32 beta_com = std::atan2(s_com, std::max(std::fabs(f_com), 1e-3f));

  // --- wing halves: strip theory at each half's aerodynamic centre ---
  // Each half is evaluated at its OWN point velocity (GetPointVelocity), so
  // roll/pitch rates change the local angle of attack: this yields natural roll
  // damping, and near the stall a rolling perturbation stalls the down-going
  // wing first (drop-a-wing / incipient spin). Aileron adds a +/- ΔCL camber
  // bias per half; + roll input rolls right (more lift left, less right).
  const f32 half_area = 0.5f * desc_.wing_area_m2;
  const f32 aspect_ratio = desc_.wing_span_m * desc_.wing_span_m / desc_.wing_area_m2;
  const f32 quarter_span = 0.25f * desc_.wing_span_m;
  const f32 flap_cl = desc_.flap_delta_cl * flaps_;
  const f32 ail = input.roll * desc_.aileron_authority;

  auto apply_wing_half = [&](f32 span_x, f32 cl_bias, bool* stalled) {
    const Vec3 ac_local{span_x, 0.15f, 0.0f};
    const Vec3 ac_world = pos + Rotate(q, ac_local);
    const Vec3 v = world_.GetPointVelocity(body_, ac_world) - wind_;
    const f32 vlen = Length(v);
    if (vlen < 1e-3f) {
      *stalled = false;
      return;
    }
    const f32 fc = Dot(v, fwd);
    const f32 uc = Dot(v, up);
    const f32 alpha = std::atan2(-uc, fc);

    // Attached linear curve (with flap + aileron camber) blended into a
    // flat-plate curve past the stall; drag rises with the flat-plate term.
    const f32 cl_attached = desc_.wing_cl_alpha * (alpha - desc_.wing_zero_lift_alpha_rad) +
                            flap_cl + cl_bias;
    const f32 cl_flat = std::sin(2.0f * alpha);  // 2 sin a cos a, flat plate
    const f32 blend = StallBlend(alpha, desc_.wing_stall_alpha_rad, desc_.post_stall_decay);
    const f32 cl = (1.0f - blend) * cl_attached + blend * cl_flat;
    *stalled = blend > 0.5f;

    // Induced drag from this half's lift; parasitic drag is applied once at the
    // CoM below. Post-stall separation adds a flat-plate drag bump.
    const f32 cd_induced = cl_attached * cl_attached / (kPi * aspect_ratio * desc_.oswald_efficiency);
    const f32 cd = (1.0f - blend) * cd_induced + blend * (0.15f + 2.0f * std::sin(alpha) * std::sin(alpha));

    const f32 qh = 0.5f * kAirDensity * vlen * vlen;
    const f32 lift = qh * half_area * cl;
    const f32 drag = qh * half_area * cd;

    // Lift acts perpendicular to the local wind in the wing's vertical plane;
    // Cross(v, left) points "up" in level flight (see aircraft.h frame notes).
    const Vec3 vdir = v * (1.0f / vlen);
    Vec3 lift_dir = Cross(v, left);
    const f32 llen = Length(lift_dir);
    if (llen > 1e-4f) lift_dir = lift_dir * (1.0f / llen);
    const Vec3 force = lift_dir * lift - vdir * drag;
    world_.AddForceAtPoint(body_, force * aero_fade, ac_world);
  };

  bool stalled_left = false, stalled_right = false;
  apply_wing_half(+quarter_span, +ail, &stalled_left);   // left wing (+X)
  apply_wing_half(-quarter_span, -ail, &stalled_right);  // right wing (-X)

  // --- parasitic + flap drag, once at the CoM ---
  if (speed > 1e-3f) {
    const f32 cd0 = desc_.cd0 + desc_.flap_delta_cd * flaps_;
    const f32 drag = q_com * desc_.wing_area_m2 * cd0;
    const Vec3 vdir = vair_com * (1.0f / speed);
    world_.AddForce(body_, vdir * (-drag) * aero_fade);
  }

  // --- fuselage side drag (sideslip): damps lateral sliding through the air ---
  {
    const f32 vside = Dot(vair_com, right);
    const f32 fside = -0.5f * kAirDensity * std::fabs(vside) * vside * desc_.fuselage_side_cd *
                      desc_.fuselage_side_area_m2;
    world_.AddForce(body_, right * (fside * aero_fade));
  }

  // --- horizontal tail (elevator): pitch control + static stability ---
  // Sampled at the tail's own point velocity so a pitch rate changes tail alpha
  // (pitch damping) and, being aft of the CoM, an alpha increase pitches the
  // nose back down (weathervane in pitch). + pitch input = nose up = tail
  // pushes down.
  {
    const Vec3 tail_local{0.0f, 0.25f, -desc_.tail_arm_m};
    const Vec3 tail_world = pos + Rotate(q, tail_local);
    const Vec3 v = world_.GetPointVelocity(body_, tail_world) - wind_;
    const f32 vlen = Length(v);
    if (vlen > 1e-3f) {
      const f32 fc = Dot(v, fwd);
      const f32 uc = Dot(v, up);
      const f32 alpha_t = std::atan2(-uc, fc);
      const f32 cl_t = desc_.tail_cl_alpha * alpha_t - desc_.elevator_authority * input.pitch;
      const f32 qt = 0.5f * kAirDensity * vlen * vlen;
      const f32 lift = qt * desc_.tail_area_m2 * cl_t;
      Vec3 lift_dir = Cross(v, left);
      const f32 llen = Length(lift_dir);
      if (llen > 1e-4f) lift_dir = lift_dir * (1.0f / llen);
      world_.AddForceAtPoint(body_, lift_dir * (lift * aero_fade), tail_world);
    }
  }

  // --- vertical fin (rudder): yaw control + weathervane yaw stability ---
  {
    const Vec3 fin_local{0.0f, 0.45f, -desc_.fin_arm_m};
    const Vec3 fin_world = pos + Rotate(q, fin_local);
    const Vec3 v = world_.GetPointVelocity(body_, fin_world) - wind_;
    const f32 vlen = Length(v);
    if (vlen > 1e-3f) {
      const f32 fc = Dot(v, fwd);
      const f32 sc = Dot(v, right);
      const f32 beta_f = std::atan2(sc, std::max(std::fabs(fc), 1e-3f));
      // Side force opposes sideslip (weathervane) and responds to rudder. + yaw
      // input yaws the nose right.
      const f32 cy = -desc_.fin_cl_beta * beta_f + desc_.rudder_authority * input.yaw;
      const f32 qf = 0.5f * kAirDensity * vlen * vlen;
      const f32 side = qf * desc_.fin_area_m2 * cy;
      world_.AddForceAtPoint(body_, right * (side * aero_fade), fin_world);
    }
  }

  // --- rotational aerodynamic damping (safety net over the strip theory) ---
  {
    const f32 qscale = Clampf(q_com / kQRef, 0.0f, 4.0f);
    const f32 mroll = -desc_.roll_damp * Dot(omega, fwd) * qscale;
    const f32 mpitch = -desc_.pitch_damp * Dot(omega, left) * qscale;
    const f32 myaw = -desc_.yaw_damp * Dot(omega, up) * qscale;
    world_.AddTorque(body_, fwd * mroll + left * mpitch + up * myaw);
  }

  // --- propulsion ---
  f32 thrust = 0.0f;
  f32 engine_load = 0.0f;
  f32 telemetry_rpm = 0.0f;
  if (desc_.propulsion == AircraftDesc::Propulsion::kProp) {
    const f32 idle_frac = Clampf(desc_.prop_idle_rpm / std::max(desc_.prop_max_rpm, 1.0f), 0.0f, 1.0f);
    const f32 target = std::max(idle_frac, Clampf(input.throttle, 0.0f, 1.0f));
    const f32 tau = std::max(desc_.engine_spool_time_s, 1e-3f);
    engine_spin_ += (target - engine_spin_) * Clampf(dt / tau, 0.0f, 1.0f);
    // Momentum-theory-flavoured thrust: power/velocity, capped near static.
    // Shaft power of a fixed-pitch prop grows ~ rpm^3, so idle thrust is tiny
    // (a parked plane at throttle 0 barely creeps) while full power is unchanged.
    const f32 power = desc_.prop_max_power_w * engine_spin_ * engine_spin_ * engine_spin_;
    const f32 v_eff = std::max(speed, desc_.prop_min_airspeed_mps);
    const f32 cap = desc_.prop_static_thrust_cap_n * engine_spin_;
    thrust = std::min(power * desc_.prop_efficiency / v_eff, cap);
    engine_load = Clampf(thrust / std::max(desc_.prop_static_thrust_cap_n, 1.0f), 0.0f, 1.0f);
    telemetry_rpm = engine_spin_ * desc_.prop_max_rpm;
  } else {
    const f32 target = Clampf(input.throttle, 0.0f, 1.0f);
    const f32 tau = std::max(desc_.jet_spool_time_s, 1e-3f);
    engine_spin_ += (target - engine_spin_) * Clampf(dt / tau, 0.0f, 1.0f);
    thrust = desc_.jet_max_thrust_n * engine_spin_;
    engine_load = engine_spin_;
    telemetry_rpm = engine_spin_ * 100.0f;  // N1 %
  }
  // Thrust acts along the body forward through the CoM (no thrust-line pitch
  // couple; keeps the model simple and the trim clean).
  world_.AddForce(body_, fwd * thrust);

  // --- landing gear: per-wheel suspension + tire friction ---
  bool on_ground = false;
  for (u32 i = 0; i < 3; ++i) {
    const AircraftDesc::Wheel& w = desc_.wheels[i];
    state_.gear_compression[i] = 0.0f;

    const Vec3 attach = pos + Rotate(q, w.local_pos);
    const Vec3 down{0, -1, 0};  // suspension travels along world down (gravity)
    const f32 reach = w.travel + w.radius;
    PhysicsWorld::RayHit hit;
    if (!world_.Raycast(attach, down, reach + 0.02f, &hit)) continue;

    const f32 compression = reach - hit.distance;  // >0 when the tire touches
    if (compression <= 0.0f) continue;
    const f32 comp = Clampf(compression, 0.0f, w.travel);
    on_ground = true;
    state_.gear_compression[i] = comp / std::max(w.travel, 1e-3f);

    const Vec3 contact = hit.position;
    Vec3 n = hit.normal;
    if (Length(n) < 1e-3f) n = Vec3{0, 1, 0};
    n = Normalize(n);

    const Vec3 pv = world_.GetPointVelocity(body_, contact);
    const f32 vn = Dot(pv, n);  // + = extending (separating) along the normal

    // Suspension spring-damper; never pulls the wheel toward the ground.
    const f32 f_spring = w.spring * comp;
    const f32 f_damp = -w.damper * vn;
    const f32 fn = std::max(f_spring + f_damp, 0.0f);
    Vec3 gear_force = n * fn;

    // Ground-plane tire frame: rolling direction = body forward projected onto
    // the contact plane; nose wheel steers it by the yaw command (reduced with
    // speed so it does not tank-turn at taxi power).
    Vec3 roll_dir = fwd - n * Dot(fwd, n);
    if (Length(roll_dir) < 1e-3f) roll_dir = right - n * Dot(right, n);
    roll_dir = Normalize(roll_dir);
    if (w.steerable) {
      const f32 speed_scale = 1.0f - SmoothStep(3.0f, 25.0f, speed) * 0.85f;
      const f32 steer = input.yaw * desc_.nose_steer_angle_rad * speed_scale;
      roll_dir = Normalize(Rotate(QuatFromAxisAngle(n, steer), roll_dir));
    }
    Vec3 lat_dir = Cross(n, roll_dir);
    if (Length(lat_dir) < 1e-3f) lat_dir = left;
    lat_dir = Normalize(lat_dir);

    const f32 v_roll = Dot(pv, roll_dir);
    const f32 v_lat = Dot(pv, lat_dir);
    constexpr f32 kSlipRef = 1.5f;  // smooths the friction sign through zero

    // Lateral tire friction (keeps it tracking straight), capped at mu*load.
    const f32 f_lat = -w.lateral_grip * fn * Clampf(v_lat / kSlipRef, -1.0f, 1.0f);
    // Rolling resistance + brakes oppose the rolling direction; the smoothed
    // clamp prevents them from reversing the plane at a standstill.
    const f32 brake = w.braked ? w.brake_force * Clampf(input.brakes, 0.0f, 1.0f) : 0.0f;
    const f32 f_long =
        -Clampf(v_roll / kSlipRef, -1.0f, 1.0f) * (w.rolling_resistance * fn + brake);

    Vec3 tire = lat_dir * f_lat + roll_dir * f_long;
    // Friction circle: total tire force cannot exceed the tire's grip budget.
    const f32 budget = 1.3f * fn;
    const f32 tlen = Length(tire);
    if (tlen > budget && tlen > 1e-4f) tire = tire * (budget / tlen);
    gear_force += tire;

    world_.AddForceAtPoint(body_, gear_force, contact);
  }

  // --- telemetry snapshot ---
  state_.airspeed_mps = speed;
  state_.vertical_speed_mps = vel.y;
  state_.alpha_deg = aero_fade > 0.0f ? alpha_com * kRad2Deg : 0.0f;
  state_.beta_deg = aero_fade > 0.0f ? beta_com * kRad2Deg : 0.0f;
  state_.stalled_left = stalled_left;
  state_.stalled_right = stalled_right;
  state_.rpm = telemetry_rpm;
  state_.engine_load = engine_load;
  state_.throttle = engine_spin_;
  state_.on_ground = on_ground;
  state_.total_mass_kg = total_mass_;
  state_.over_mtom = total_mass_ > desc_.max_takeoff_mass_kg;
  state_.position = pos;
  state_.rotation = q;
}

}  // namespace rx::physics
