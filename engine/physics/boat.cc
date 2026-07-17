#include "physics/boat.h"

#include <algorithm>
#include <cmath>

namespace rx::physics {

namespace {

constexpr f32 kWaterDensity = 1000.0f;  // kg/m^3, fresh water
constexpr f32 kGravity = 9.81f;         // m/s^2

f32 Clamp(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Cubic smoothstep of x over [edge0, edge1] -> [0,1]. Degenerate edges give a
// hard step so callers can't produce a NaN.
f32 SmoothStep(f32 edge0, f32 edge1, f32 x) {
  if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
  const f32 t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Yaw quaternion about +Y (x,y,z,w), for spawn orientation.
Quat YawQuat(f32 yaw_radians) {
  return {0.0f, std::sin(yaw_radians * 0.5f), 0.0f, std::cos(yaw_radians * 0.5f)};
}

// v * |v|: signed quadratic term (drag ~ v^2 but keeps the sign of v).
f32 SignedSquare(f32 v) { return v * std::fabs(v); }

// Guards a force/torque against NaN/Inf before it reaches the solver.
bool Finite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

Boat::Boat(PhysicsWorld& world, const BoatDesc& desc, const Vec3& position, f32 yaw_radians)
    : world_(world), desc_(desc) {
  // Mass comes from the box density: density = mass / box volume, so Jolt
  // derives exactly desc.mass. Buoyancy and drag are our model's job, not the
  // shape's, so the shape density only sets the mass here.
  const Vec3& he = desc_.hull_half_extent;
  const f32 volume = std::max(8.0f * he.x * he.y * he.z, 1e-3f);
  const f32 density = desc_.mass / volume;
  const Quat q = YawQuat(yaw_radians);
  const f32 rot[4] = {q.x, q.y, q.z, q.w};
  body_ = world_.AddDynamicBox(position, he, density, Vec3{});
  if (body_ == 0) return;  // stub / spawn failure
  // Orient the freshly spawned box (AddDynamicBox spawns axis-aligned).
  world_.SetBodyPosition(body_, position, rot);
  // Our multi-point hull model replaces the world's generic whole-body
  // buoyancy; opting out keeps it from being applied twice.
  world_.set_buoyancy_exempt(body_, true);
  rpm_ = desc_.idle_rpm;
  state_.position = position;
  state_.rotation = q;
}

void Boat::Update(const BoatInput& input, f32 dt) {
  if (body_ == 0 || dt <= 0.0f) return;

  const f32 throttle = Clamp(input.throttle, -1.0f, 1.0f);
  const f32 steer = Clamp(input.steer, -1.0f, 1.0f);
  const f32 trim = Clamp(input.trim, -1.0f, 1.0f);

  // --- pose + body axes ---
  Vec3 pos{};
  f32 rot[4] = {0, 0, 0, 1};
  world_.GetBodyTransform(body_, &pos, rot);
  const Quat q{rot[0], rot[1], rot[2], rot[3]};
  const Vec3 forward = Rotate(q, Vec3{0, 0, 1});
  const Vec3 right = Rotate(q, Vec3{1, 0, 0});
  const Vec3 up = Rotate(q, Vec3{0, 1, 0});
  const f32 weight = desc_.mass * kGravity;

  // --- engine spool: rpm chases the throttle target with a first-order lag ---
  const f32 target_rpm =
      desc_.idle_rpm + std::fabs(throttle) * (desc_.max_rpm - desc_.idle_rpm);
  const f32 spool = desc_.spool_time > 0 ? (1.0f - std::exp(-dt / desc_.spool_time)) : 1.0f;
  rpm_ += (target_rpm - rpm_) * spool;
  const f32 rpm_span = std::max(desc_.max_rpm - desc_.idle_rpm, 1.0f);
  const f32 thrust_frac = Clamp((rpm_ - desc_.idle_rpm) / rpm_span, 0.0f, 1.0f);
  // Signed thrust magnitude: astern is throttled down by reverse_fraction.
  f32 thrust_mag = desc_.max_thrust * thrust_frac;
  if (throttle < 0.0f) thrust_mag = -thrust_mag * desc_.reverse_fraction;

  // --- volumetric multi-point hull buoyancy ---
  // Each sample owns an equal share of the hull volume (subvol) and, while
  // submerged, displaces it: F = rho * g * subvol (N, up) applied AT the
  // sample. A `frac` ramps the topmost partially submerged layer over one
  // layer thickness so the heave spring is continuous (frac also caps the
  // waterplane stiffness at rho*g*waterplane_area, independent of grid_height).
  // A per-sample vertical damper bleeds heave/roll/pitch so the springs settle
  // without ringing. Because buoyancy is distributed through the volume, the
  // centre of buoyancy migrates to the submerged side and rights the hull.
  const u32 nx = std::max<u32>(desc_.grid_beam, 1);
  const u32 ny = std::max<u32>(desc_.grid_height, 1);
  const u32 nz = std::max<u32>(desc_.grid_len, 1);
  const Vec3& he = desc_.hull_half_extent;
  const f32 hull_volume = 8.0f * he.x * he.y * he.z;
  const f32 subvol = hull_volume / static_cast<f32>(nx * ny * nz);
  const f32 layer_thickness = std::max(2.0f * he.y / static_cast<f32>(ny), 1e-3f);
  const f32 buoy_per_sample = kWaterDensity * kGravity * subvol;
  u32 bottom_wet = 0;  // submerged samples in the bottom layer -> wetted/planing
  const u32 bottom_total = nx * nz;
  for (u32 iy = 0; iy < ny; ++iy) {
    const f32 ty = ny > 1 ? (static_cast<f32>(iy) / (ny - 1)) * 2.0f - 1.0f : -1.0f;
    for (u32 iz = 0; iz < nz; ++iz) {
      // Cell-centre parametric coords in [-1,1] across beam/height/length.
      const f32 tz = nz > 1 ? (static_cast<f32>(iz) / (nz - 1)) * 2.0f - 1.0f : 0.0f;
      for (u32 ix = 0; ix < nx; ++ix) {
        const f32 tx = nx > 1 ? (static_cast<f32>(ix) / (nx - 1)) * 2.0f - 1.0f : 0.0f;
        const Vec3 local{tx * he.x, ty * he.y, tz * he.z};
        const Vec3 world = pos + Rotate(q, local);
        f32 surface_h = 0.0f;
        Vec3 flow{};
        if (!world_.SampleWater(world, &surface_h, &flow)) continue;
        const f32 depth = surface_h - world.y;
        if (depth <= 0.0f) continue;
        const f32 frac = Clamp(depth / layer_thickness, 0.0f, 1.0f);
        if (iy == 0) ++bottom_wet;  // bottom layer feeds the wetted fraction
        const f32 buoyancy = buoy_per_sample * frac;
        const Vec3 point_vel = world_.GetPointVelocity(body_, world);
        // Damp vertical motion relative to the (approximately static) surface;
        // the water-height callback exposes no surface vertical velocity, so
        // absolute vy is used - slightly over-damped on steep chop, always
        // stable.
        const f32 damp = -desc_.heave_damping * frac * point_vel.y;
        const Vec3 f_point{0.0f, buoyancy + damp, 0.0f};
        if (Finite(f_point)) world_.AddForceAtPoint(body_, f_point, world);
      }
    }
  }
  const f32 wetted =
      bottom_total > 0 ? static_cast<f32>(bottom_wet) / static_cast<f32>(bottom_total) : 0.0f;

  // --- hull drag, relative to the water flow (rivers carry the hull) ---
  Vec3 vel = world_.GetLinearVelocity(body_);
  f32 centre_h = 0.0f;
  Vec3 flow{};
  world_.SampleWater(pos, &centre_h, &flow);  // flow is horizontal
  const Vec3 vrel = vel - flow;
  const f32 v_fwd = Dot(vrel, forward);
  const f32 v_lat = Dot(vrel, right);
  const f32 v_vert = Dot(vel, up);
  const f32 speed = std::sqrt(vel.x * vel.x + vel.z * vel.z);

  // Planing fraction from forward speed; drives bow lift + drag drop.
  const f32 planing = SmoothStep(desc_.hull_speed, desc_.plane_full_speed, std::fabs(v_fwd));

  // Longitudinal: fore drag < aft drag; wetted-scaled; dropped by planing when
  // moving ahead. Lateral: strong keel drag so the hull carves rather than
  // slides. Vertical: whole-hull heave drag.
  const f32 c_long = v_fwd >= 0.0f ? desc_.drag_fwd : desc_.drag_aft;
  const f32 plane_drop = v_fwd >= 0.0f ? (1.0f - desc_.plane_drag_reduction * planing) : 1.0f;
  const f32 drag_long = -c_long * SignedSquare(v_fwd) * wetted * plane_drop;
  const f32 drag_lat = -desc_.drag_lateral * SignedSquare(v_lat) * wetted;
  const f32 drag_vert = -desc_.drag_vertical * SignedSquare(v_vert);
  Vec3 hull_force = forward * drag_long + right * drag_lat + up * drag_vert;
  if (Finite(hull_force)) world_.AddForce(body_, hull_force);

  // --- planing lift: past hull speed the bow lifts, reducing wetted drag ---
  // Dynamic lift = plane_lift * v_fwd^2 * planing, capped at plane_lift_cap of
  // the weight and GATED BY the wetted fraction: as the hull rises the wetted
  // area falls, cutting the lift, so lift transfers load off buoyancy without
  // launching the hull clear of the water (a boat that flies off is no boat).
  // Applied at a modest forward lever so the bow rises a few degrees.
  if (planing > 0.0f && desc_.plane_lift > 0.0f && v_fwd > 0.0f && wetted > 0.0f) {
    f32 lift = desc_.plane_lift * v_fwd * v_fwd * planing;
    lift = std::min(lift, desc_.plane_lift_cap * weight) * wetted;
    const Vec3 bow = pos + Rotate(q, Vec3{0.0f, -he.y, he.z * 0.35f});
    const Vec3 lift_force{0.0f, lift, 0.0f};  // world up: lifts and pitches bow up
    if (Finite(lift_force)) world_.AddForceAtPoint(body_, lift_force, bow);
  }

  // --- propulsion: thrust at the prop point, only while it is submerged ---
  const Vec3 prop = pos + Rotate(q, desc_.prop_offset);
  f32 prop_surface = 0.0f;
  Vec3 prop_flow{};
  bool prop_submerged = false;
  if (world_.SampleWater(prop, &prop_surface, &prop_flow)) {
    prop_submerged = prop.y < prop_surface;
  }
  if (prop_submerged && thrust_mag != 0.0f) {
    const Vec3 thrust = forward * thrust_mag;
    if (Finite(thrust)) world_.AddForceAtPoint(body_, thrust, prop);
  }

  // --- rudder: stern sideforce from water speed + prop wash ---
  // Authority scales with the water speed over the rudder (|v_fwd|^2) plus the
  // propeller wash (proportional to thrust), so the boat still answers the helm
  // on the wash at a standstill. Only the wash term needs the prop submerged.
  const f32 wash = (prop_submerged ? desc_.rudder_wash_gain * std::fabs(thrust_mag) : 0.0f);
  const f32 rudder_mag = steer * (desc_.rudder_speed_gain * v_fwd * v_fwd + wash);
  if (rudder_mag != 0.0f) {
    const Vec3 rudder_pt = pos + Rotate(q, desc_.rudder_offset);
    const Vec3 rudder_force = right * rudder_mag;
    if (Finite(rudder_force)) world_.AddForceAtPoint(body_, rudder_force, rudder_pt);
  }

  // --- ballast keel righting couple (emulated lowered CoM) ---
  // Relocating the weight to a point com_drop below the geometric centre adds a
  // couple torque = r_ballast x (m*g down). Zero when upright, righting when
  // heeled; the dominant self-righting term from a knock-down.
  if (desc_.com_drop != 0.0f) {
    const Vec3 r_ballast = up * (-desc_.com_drop);
    const Vec3 gravity_force{0.0f, -weight, 0.0f};
    const Vec3 righting = Cross(r_ballast, gravity_force);
    if (Finite(righting)) world_.AddTorque(body_, righting);
  }

  // --- yaw-rate damping + trim ---
  const Vec3 ang = world_.GetAngularVelocity(body_);
  Vec3 torque = up * (-desc_.yaw_damping * Dot(ang, up));
  if (trim != 0.0f) torque += right * (trim * desc_.trim_torque);
  if (Finite(torque)) world_.AddTorque(body_, torque);

  // --- telemetry ---
  state_.rpm = rpm_;
  state_.engine_load = thrust_frac;
  state_.throttle = throttle;
  state_.speed_mps = speed;
  state_.forward_speed = v_fwd;
  state_.planing = planing;
  state_.wetted = wetted;
  state_.prop_submerged = prop_submerged;
  state_.position = pos;
  state_.rotation = q;
}

}  // namespace rx::physics
