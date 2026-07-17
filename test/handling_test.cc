// Headless proof that the vehicle handling profiles (engine/physics/
// vehicle_profiles.h) genuinely differ, over the real Jolt vehicle path (no
// GPU). Each scenario builds its own flat height-field world so the tagged
// ground surface is fixed, drives a profile through a measured manoeuvre, and
// asserts an ORDERING between profiles (not an absolute magnitude), printing
// every number. Tune the presets until the orderings hold with honest margins.
//
// Checks:
//   (a) 0-100 km/h time, strictly ordered sports<muscle<hatch<SUV<van<semi
//   (b) 100-0 km/h braking distance, sports shortest, semi longest by a mile
//   (c) step-steer at 80 km/h: roll ordering + heavy rigs shed more speed
//   (d) grass launch: AWD SUV reaches 40 km/h quicker than RWD muscle
//   (e) FWD understeer (front slip > rear) vs RWD oversteer (rear > front)
//   (f) downforce: the sports car's grip grows with speed, the hatch's doesn't
//   (g) every profile is NaN-free, settles at rest, and respects surface grip

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "core/math.h"
#include "physics/physics_world.h"
#include "physics/vehicle_profiles.h"

using namespace rx;
using physics::PhysicsWorld;
using physics::SurfaceType;
using physics::VehicleId;
using Desc = PhysicsWorld::VehicleDesc;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kKmh = 1.0f / 3.6f;  // km/h -> m/s

int g_failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "handling_test FAIL: %s\n", what);
    ++g_failures;
  }
}

physics::BodyId AddFlatGround(PhysicsWorld& world, SurfaceType surface) {
  // Large enough that even the semi's minute-long 0-100 run and its long stop
  // stay on the sheet (a small field let fast runs drive off the edge and the
  // vehicle fell into the void). Flat, so the coarse sampling is exact.
  constexpr u32 kSamples = 96;
  constexpr f32 kSize = 24000.0f;
  std::vector<f32> heights(static_cast<size_t>(kSamples) * kSamples, 0.0f);
  return world.AddHeightField(Vec3{-kSize * 0.5f, 0.0f, -kSize * 0.5f}, heights.data(), kSamples,
                              kSize, surface);
}

VehicleId SpawnSettled(PhysicsWorld& world, const Desc& d) {
  const f32 y = d.half_extent.y + d.suspension_max + d.wheel_radius + 0.5f;
  const VehicleId id = world.CreateVehicle(d, Vec3{0, y, 0}, 0.0f);
  for (int i = 0; i < 150; ++i) {
    world.DriveVehicle(id, 0.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
  }
  return id;
}

f32 Heading(const f32 q[4]) {
  const Vec3 f = Rotate(Quat{q[0], q[1], q[2], q[3]}, Vec3{0, 0, 1});
  return std::atan2(f.x, f.z);
}

f32 RollAngle(const f32 q[4]) {
  const Vec3 right = Rotate(Quat{q[0], q[1], q[2], q[3]}, Vec3{1, 0, 0});
  return std::asin(std::clamp(right.y, -1.0f, 1.0f));
}

f32 WrapPi(f32 a) {
  while (a > 3.14159265f) a -= 6.2831853f;
  while (a < -3.14159265f) a += 6.2831853f;
  return a;
}

// Full throttle from rest until `target` m/s; returns time in seconds (a big
// sentinel if never reached within max_s).
f32 ZeroToSpeed(const Desc& d, f32 target, SurfaceType surface, f32 max_s) {
  PhysicsWorld world;
  if (!world.Initialize()) return -1;
  AddFlatGround(world, surface);
  VehicleId id = SpawnSettled(world, d);
  const int cap = static_cast<int>(max_s * 60.0f);
  for (int i = 0; i < cap; ++i) {
    world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
    if (world.VehicleForwardSpeed(id) >= target) return static_cast<f32>(i + 1) * kDt;
  }
  return max_s + 100.0f;  // sentinel: did not reach it
}

// Accelerate to `entry` m/s, then full brake to rest; returns the braking
// distance (metres travelled from brake application to stop).
f32 BrakeDistance(const Desc& d, f32 entry) {
  PhysicsWorld world;
  if (!world.Initialize()) return -1;
  AddFlatGround(world, SurfaceType::kAsphalt);
  VehicleId id = SpawnSettled(world, d);
  for (int i = 0; i < 60 * 90; ++i) {
    world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
    if (world.VehicleForwardSpeed(id) >= entry) break;
  }
  Vec3 start{};
  f32 rot[4];
  world.GetVehicleTransform(id, &start, rot);
  Vec3 last = start;
  for (int i = 0; i < 60 * 30; ++i) {
    world.DriveVehicle(id, 0.0f, 0.0f, 1.0f, 0.0f);
    world.Update(kDt);
    world.GetVehicleTransform(id, &last, rot);
    if (std::fabs(world.VehicleForwardSpeed(id)) < 0.5f) break;
  }
  const f32 dx = last.x - start.x, dz = last.z - start.z;
  return std::sqrt(dx * dx + dz * dz);
}

struct SteerResult {
  f32 peak_roll_deg = 0;
  f32 speed_lost_mps = 0;
  f32 heading_change_deg = 0;
};

// Accelerate to `entry`, then full-lock steer with light throttle for ~2.5 s.
SteerResult StepSteer(const Desc& d, f32 entry) {
  SteerResult r;
  PhysicsWorld world;
  if (!world.Initialize()) return r;
  AddFlatGround(world, SurfaceType::kAsphalt);
  VehicleId id = SpawnSettled(world, d);
  for (int i = 0; i < 60 * 90; ++i) {
    world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
    if (world.VehicleForwardSpeed(id) >= entry) break;
  }
  const f32 entry_speed = world.VehicleForwardSpeed(id);
  f32 rot[4];
  Vec3 pos{};
  world.GetVehicleTransform(id, &pos, rot);
  const f32 start_heading = Heading(rot);
  f32 min_speed = entry_speed;
  for (int i = 0; i < 150; ++i) {
    // A gentle steer step: a hard step at 80 km/h tips every tall vehicle fully
    // onto its side (a saturated ~60 deg), hiding the ordering. A light input
    // keeps them in the graded body-lean regime.
    world.DriveVehicle(id, 0.35f, 0.30f, 0.0f, 0.0f);
    world.Update(kDt);
    world.GetVehicleTransform(id, &pos, rot);
    r.peak_roll_deg = std::max(r.peak_roll_deg, std::fabs(RollAngle(rot)) * 57.2958f);
    min_speed = std::min(min_speed, std::fabs(world.VehicleForwardSpeed(id)));
  }
  world.GetVehicleTransform(id, &pos, rot);
  r.heading_change_deg = std::fabs(WrapPi(Heading(rot) - start_heading)) * 57.2958f;
  r.speed_lost_mps = entry_speed - min_speed;
  return r;
}

struct SlipResult {
  f32 front = 0;
  f32 rear = 0;
};

// Full throttle into a full-lock corner at ~40 km/h; returns the steady-state
// mean lateral slip angle (rad) of the front and rear axles.
SlipResult CornerSlip(const Desc& d) {
  SlipResult r;
  PhysicsWorld world;
  if (!world.Initialize()) return r;
  AddFlatGround(world, SurfaceType::kAsphalt);
  VehicleId id = SpawnSettled(world, d);
  for (int i = 0; i < 60 * 30; ++i) {
    world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
    if (world.VehicleForwardSpeed(id) >= 14.0f) break;
  }
  // Full throttle into a moderate (not full-lock) steer: a full lock buries the
  // steered front wheels in geometric slip and hides the balance. The peak axle
  // slip over the slide shows which end lets go - front (understeer, the FWD
  // hatch) or rear (power oversteer, the RWD muscle car).
  for (int i = 0; i < 180; ++i) {
    world.DriveVehicle(id, 1.0f, 0.45f, 0.0f, 0.0f);
    world.Update(kDt);
    if (i < 40) continue;  // let the corner develop
    PhysicsWorld::VehicleState st;
    if (!world.GetVehicleState(id, &st)) continue;
    const f32 f = 0.5f * (std::fabs(st.wheels[0].lateral_slip) + std::fabs(st.wheels[1].lateral_slip));
    const f32 rr = 0.5f * (std::fabs(st.wheels[2].lateral_slip) + std::fabs(st.wheels[3].lateral_slip));
    r.front = std::max(r.front, f);
    r.rear = std::max(r.rear, rr);
  }
  return r;
}

// Peak steady-state lateral acceleration (m/s^2) in a hard turn held near
// `target` m/s. a_lat = speed * yaw_rate: with aero downforce the tyre holds
// more load at speed, so the achievable a_lat grows with speed.
f32 LateralAccel(const Desc& d, f32 target) {
  PhysicsWorld world;
  if (!world.Initialize()) return -1;
  AddFlatGround(world, SurfaceType::kAsphalt);
  VehicleId id = SpawnSettled(world, d);
  for (int i = 0; i < 60 * 120; ++i) {
    world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
    if (world.VehicleForwardSpeed(id) >= target) break;
  }
  f32 rot[4];
  Vec3 pos{};
  world.GetVehicleTransform(id, &pos, rot);
  f32 prev_heading = Heading(rot);
  f32 peak = 0;
  for (int i = 0; i < 90; ++i) {  // ~1.5 s; grab the peak before speed bleeds far
    const f32 spd = world.VehicleForwardSpeed(id);
    const f32 thr = spd < target ? 0.7f : 0.0f;
    world.DriveVehicle(id, thr, 1.0f, 0.0f, 0.0f);
    world.Update(kDt);
    world.GetVehicleTransform(id, &pos, rot);
    const f32 h = Heading(rot);
    const f32 yaw_rate = std::fabs(WrapPi(h - prev_heading)) / kDt;
    prev_heading = h;
    if (i < 8) continue;  // skip the initial transient
    peak = std::max(peak, std::fabs(world.VehicleForwardSpeed(id)) * yaw_rate);
  }
  return peak;
}

bool Finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

}  // namespace

int main() {
  PhysicsWorld probe;
  if (!probe.Initialize()) {
    std::fprintf(stderr, "handling_test: physics unavailable, skipping\n");
    return 0;
  }

  const Desc sports = physics::SportsCarProfile();
  const Desc muscle = physics::MuscleCarProfile();
  const Desc hatch = physics::HatchbackProfile();
  const Desc suv = physics::SuvProfile();
  const Desc van = physics::VanProfile(1.0f);  // fully laden
  const Desc semi = physics::SemiTruckProfile();

  // (a) 0-100 km/h, strictly ordered.
  {
    const f32 t100 = 100.0f * kKmh;
    const f32 s = ZeroToSpeed(sports, t100, SurfaceType::kAsphalt, 20.0f);
    const f32 m = ZeroToSpeed(muscle, t100, SurfaceType::kAsphalt, 20.0f);
    const f32 h = ZeroToSpeed(hatch, t100, SurfaceType::kAsphalt, 40.0f);
    const f32 u = ZeroToSpeed(suv, t100, SurfaceType::kAsphalt, 45.0f);
    const f32 v = ZeroToSpeed(van, t100, SurfaceType::kAsphalt, 60.0f);
    const f32 sm = ZeroToSpeed(semi, t100, SurfaceType::kAsphalt, 90.0f);
    std::fprintf(stderr,
                 "(a) 0-100 km/h: sports=%.2fs muscle=%.2fs hatch=%.2fs suv=%.2fs van=%.2fs "
                 "semi=%.2fs\n",
                 s, m, h, u, v, sm);
    // Every profile must actually REACH 100 km/h within its own timeout;
    // ZeroToSpeed returns max_s+100 on a miss, which would otherwise satisfy the
    // strict ordering below (a slower vehicle's larger timeout > a faster one's)
    // and hide a profile that never got there. Assert reached, per profile.
    Check(s <= 20.0f, "(a) sports never reached 100 km/h");
    Check(m <= 20.0f, "(a) muscle never reached 100 km/h");
    Check(h <= 40.0f, "(a) hatch never reached 100 km/h");
    Check(u <= 45.0f, "(a) SUV never reached 100 km/h");
    Check(v <= 60.0f, "(a) van never reached 100 km/h");
    Check(sm <= 90.0f, "(a) semi never reached 100 km/h");
    Check(s < m, "(a) sports not quicker than muscle");
    Check(m < h, "(a) muscle not quicker than hatch");
    Check(h < u, "(a) hatch not quicker than SUV");
    Check(u < v, "(a) SUV not quicker than van");
    Check(v < sm, "(a) van not quicker than semi");
    // Honest margin over machine/compiler FP variance (the multithreaded Jolt
    // solver is deterministic run-to-run for a fixed call order, but SSE/NEON and
    // build flags shift absolute times across machines). ~6.0 s here; < 7.0 still
    // proves the sports car is the quick benchmark without a hair-trigger.
    Check(s < 7.0f, "(a) sports 0-100 not under ~7 s");
    Check(sm > 25.0f, "(a) semi 0-100 not over ~25 s");
  }

  // (b) 100-0 km/h braking distance.
  {
    const f32 entry = 100.0f * kKmh;
    const f32 s = BrakeDistance(sports, entry);
    const f32 m = BrakeDistance(muscle, entry);
    const f32 h = BrakeDistance(hatch, entry);
    const f32 u = BrakeDistance(suv, entry);
    const f32 v = BrakeDistance(van, entry);
    const f32 sm = BrakeDistance(semi, entry);
    std::fprintf(stderr,
                 "(b) 100-0 braking: sports=%.1fm muscle=%.1fm hatch=%.1fm suv=%.1fm van=%.1fm "
                 "semi=%.1fm\n",
                 s, m, h, u, v, sm);
    Check(s < m && s < h && s < u && s < v && s < sm, "(b) sports not the shortest stop");
    Check(sm > s * 2.5f, "(b) semi stop not a wide margin longer than sports");
    Check(sm > v && sm > u, "(b) semi not the longest stop");
  }

  // (c) step-steer at 80 km/h: roll ordering + heavy rigs shed more speed.
  {
    // A ~55 km/h step: at 80 km/h the soft, tall profiles (van especially) roll
    // fully onto their side and every reading saturates near 60 deg, so the
    // graded body-lean ordering is only visible below the rollover regime.
    const f32 entry = 55.0f * kKmh;
    const SteerResult s = StepSteer(sports, entry);
    const SteerResult u = StepSteer(suv, entry);
    const SteerResult v = StepSteer(van, entry);
    const SteerResult sm = StepSteer(semi, entry);
    std::fprintf(stderr,
                 "(c) step-steer roll: sports=%.1fdeg suv=%.1fdeg van=%.1fdeg semi=%.1fdeg\n",
                 s.peak_roll_deg, u.peak_roll_deg, v.peak_roll_deg, sm.peak_roll_deg);
    std::fprintf(stderr,
                 "(c) speed lost: sports=%.1f suv=%.1f van=%.1f semi=%.1f m/s | heading chg: "
                 "sports=%.0f van=%.0f semi=%.0f deg\n",
                 s.speed_lost_mps, u.speed_lost_mps, v.speed_lost_mps, sm.speed_lost_mps,
                 s.heading_change_deg, v.heading_change_deg, sm.heading_change_deg);
    Check(s.peak_roll_deg < u.peak_roll_deg, "(c) sports rolls more than SUV");
    Check(u.peak_roll_deg < v.peak_roll_deg, "(c) SUV rolls more than van");
    Check(u.peak_roll_deg < sm.peak_roll_deg, "(c) SUV rolls more than semi");
    Check(v.speed_lost_mps > s.speed_lost_mps, "(c) van did not shed more speed than sports");
    Check(sm.speed_lost_mps > s.speed_lost_mps, "(c) semi did not shed more speed than sports");
  }

  // (d) grass launch: AWD SUV reaches 40 km/h quicker than RWD muscle.
  {
    const f32 t40 = 40.0f * kKmh;
    const f32 u = ZeroToSpeed(suv, t40, SurfaceType::kGrass, 25.0f);
    const f32 m = ZeroToSpeed(muscle, t40, SurfaceType::kGrass, 25.0f);
    std::fprintf(stderr, "(d) grass 0-40 km/h: AWD suv=%.2fs RWD muscle=%.2fs\n", u, m);
    Check(u < m * 0.85f, "(d) AWD SUV not materially quicker than RWD muscle on grass");
  }

  // (e) FWD understeer vs RWD oversteer.
  {
    const SlipResult h = CornerSlip(hatch);
    const SlipResult m = CornerSlip(muscle);
    std::fprintf(stderr,
                 "(e) corner slip (rad): hatch front=%.3f rear=%.3f | muscle front=%.3f rear=%.3f\n",
                 h.front, h.rear, m.front, m.rear);
    Check(h.front > h.rear, "(e) FWD hatch front slip not above rear (no understeer)");
    Check(m.rear > m.front, "(e) RWD muscle rear slip not above front (no oversteer)");
  }

  // (f) downforce: sports grip grows with speed, hatch's doesn't.
  {
    const f32 lo = 60.0f * kKmh;
    const f32 hi = 120.0f * kKmh;
    const f32 s_lo = LateralAccel(sports, lo);
    const f32 s_hi = LateralAccel(sports, hi);
    const f32 h_lo = LateralAccel(hatch, lo);
    const f32 h_hi = LateralAccel(hatch, hi);
    std::fprintf(stderr,
                 "(f) lat accel (m/s^2): sports 60=%.2f 120=%.2f (x%.2f) | hatch 60=%.2f 120=%.2f "
                 "(x%.2f)\n",
                 s_lo, s_hi, s_hi / s_lo, h_lo, h_hi, h_hi / h_lo);
    Check(s_hi > s_lo * 1.15f, "(f) sports grip did not grow with speed (downforce)");
    Check(s_hi / s_lo > h_hi / h_lo, "(f) sports grip did not grow more than the hatch's");
  }

  // (g) NaN-free, settles at rest, respects surfaces/wetness.
  {
    struct Named {
      const char* name;
      Desc d;
    };
    const Named all[] = {{"sports", sports}, {"muscle", muscle}, {"hatch", hatch},
                         {"suv", suv},       {"van", van},       {"semi", semi}};
    for (const Named& it : all) {
      PhysicsWorld world;
      world.Initialize();
      AddFlatGround(world, SurfaceType::kAsphalt);
      VehicleId id = SpawnSettled(world, it.d);  // 2.5 s of settling inside
      Vec3 pos{};
      f32 rot[4];
      world.GetVehicleTransform(id, &pos, rot);
      const f32 rest_speed = std::fabs(world.VehicleForwardSpeed(id));
      Check(Finite(pos), "(g) profile position went non-finite");
      Check(rest_speed < 0.5f, "(g) profile did not settle at rest");
      // Drive full throttle 2 s; must move and stay finite.
      for (int i = 0; i < 120; ++i) {
        world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
        world.Update(kDt);
      }
      world.GetVehicleTransform(id, &pos, rot);
      Check(Finite(pos) && std::isfinite(world.VehicleForwardSpeed(id)),
            "(g) profile went non-finite under throttle");
      Check(world.VehicleForwardSpeed(id) > 1.0f, "(g) profile did not accelerate");
      std::fprintf(stderr, "(g) %s: rest_speed=%.3f moved_to=%.1f m/s (finite)\n", it.name,
                   rest_speed, world.VehicleForwardSpeed(id));
    }
    // Surface respect: the sports car stops far longer on ice than asphalt, and
    // rain lengthens the asphalt stop.
    const f32 dry = BrakeDistance(sports, 80.0f * kKmh);
    f32 ice = 0, wet = 0;
    {
      PhysicsWorld world;
      world.Initialize();
      AddFlatGround(world, SurfaceType::kIce);
      VehicleId id = SpawnSettled(world, sports);
      for (int i = 0; i < 60 * 40; ++i) {
        world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
        world.Update(kDt);
        if (world.VehicleForwardSpeed(id) >= 80.0f * kKmh) break;
      }
      Vec3 a{};
      f32 r[4];
      world.GetVehicleTransform(id, &a, r);
      Vec3 b = a;
      for (int i = 0; i < 60 * 60; ++i) {
        world.DriveVehicle(id, 0.0f, 0.0f, 1.0f, 0.0f);
        world.Update(kDt);
        world.GetVehicleTransform(id, &b, r);
        if (std::fabs(world.VehicleForwardSpeed(id)) < 0.5f) break;
      }
      ice = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    }
    {
      PhysicsWorld world;
      world.Initialize();
      AddFlatGround(world, SurfaceType::kAsphalt);
      world.set_surface_wetness(1.0f);
      VehicleId id = SpawnSettled(world, sports);
      for (int i = 0; i < 60 * 40; ++i) {
        world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
        world.Update(kDt);
        if (world.VehicleForwardSpeed(id) >= 80.0f * kKmh) break;
      }
      Vec3 a{};
      f32 r[4];
      world.GetVehicleTransform(id, &a, r);
      Vec3 b = a;
      for (int i = 0; i < 60 * 40; ++i) {
        world.DriveVehicle(id, 0.0f, 0.0f, 1.0f, 0.0f);
        world.Update(kDt);
        world.GetVehicleTransform(id, &b, r);
        if (std::fabs(world.VehicleForwardSpeed(id)) < 0.5f) break;
      }
      wet = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
    }
    std::fprintf(stderr, "(g) sports 80-0: dry=%.1fm ice=%.1fm wet=%.1fm\n", dry, ice, wet);
    Check(ice > dry * 2.0f, "(g) ice not much longer than asphalt");
    Check(wet > dry * 1.1f, "(g) wetness did not lengthen the asphalt stop");
  }

  if (g_failures == 0) {
    std::fprintf(stderr, "handling_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "handling_test: %d check(s) FAILED\n", g_failures);
  return 1;
}
