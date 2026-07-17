// Headless vehicle-realism checks over the real Jolt vehicle path (no GPU):
// acceleration + auto-shift, surface-dependent braking (ice vs asphalt), rain
// wetness, manual transmission gear holding, telemetry ranges, and water
// aquaplaning. Each scenario builds its own flat height-field world so the
// tagged ground surface is fixed for the run.

#include <cmath>
#include <cstdio>
#include <vector>

#include "core/math.h"
#include "physics/physics_world.h"

using namespace rx;
using physics::PhysicsWorld;
using physics::SurfaceType;
using physics::VehicleId;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "vehicle_test FAIL: %s\n", what);
  return 1;
}

// Flat height-field ground of the given surface, large enough to drive on.
physics::BodyId AddFlatGround(PhysicsWorld& world, SurfaceType surface) {
  constexpr u32 kSamples = 64;
  constexpr f32 kSize = 1024.0f;
  std::vector<f32> heights(static_cast<size_t>(kSamples) * kSamples, 0.0f);
  return world.AddHeightField(Vec3{-kSize * 0.5f, 0.0f, -kSize * 0.5f}, heights.data(), kSamples,
                              kSize, surface);
}

PhysicsWorld::VehicleDesc DefaultCar() {
  PhysicsWorld::VehicleDesc desc;
  desc.drivetrain = PhysicsWorld::Drivetrain::kAWD;  // best launch traction for the tests
  // Brakes strong enough to lock the wheels so full-brake stops are GRIP-limited,
  // not brake-torque-limited. Jolt's stock 1500 Nm caps deceleration at ~0.6 g on
  // dry asphalt - below the tyre's ~1 g grip - so wet asphalt (~0.7 g) would still
  // out-grip the brakes and wet vs dry stops came out near-equal (a hair-trigger
  // 1.1x). With locked wheels the stop is governed by grip, so wetness lengthens
  // it by a real, machine-robust margin.
  desc.max_brake_torque = 4000.0f;
  return desc;
}

// Spawns a car a little above the ground and lets the suspension settle.
VehicleId SpawnSettledCar(PhysicsWorld& world, const PhysicsWorld::VehicleDesc& desc) {
  const VehicleId id = world.CreateVehicle(desc, Vec3{0, desc.wheel_radius + 0.6f, 0}, 0.0f);
  for (int i = 0; i < 90; ++i) {
    world.DriveVehicle(id, 0, 0, 0, 0);
    world.Update(kDt);
  }
  return id;
}

f32 HorizontalDistance(const Vec3& a, const Vec3& b) {
  const f32 dx = a.x - b.x;
  const f32 dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

// Full-throttle until the car reaches `target` m/s or `max_steps` elapse.
// Returns the achieved forward speed.
f32 DriveToSpeed(PhysicsWorld& world, VehicleId id, f32 target, int max_steps) {
  for (int i = 0; i < max_steps; ++i) {
    world.DriveVehicle(id, 1.0f, 0.0f, 0.0f, 0.0f);
    world.Update(kDt);
    if (world.VehicleForwardSpeed(id) >= target) break;
  }
  return world.VehicleForwardSpeed(id);
}

// Full brake from the current speed until the car stops (or timeout). Returns
// the horizontal distance travelled while braking.
f32 BrakeToStop(PhysicsWorld& world, VehicleId id, int max_steps) {
  Vec3 start{};
  f32 rot[4];
  world.GetVehicleTransform(id, &start, rot);
  Vec3 last = start;
  for (int i = 0; i < max_steps; ++i) {
    world.DriveVehicle(id, 0.0f, 0.0f, 1.0f, 0.0f);
    world.Update(kDt);
    world.GetVehicleTransform(id, &last, rot);
    if (std::fabs(world.VehicleForwardSpeed(id)) < 0.5f) break;
  }
  return HorizontalDistance(start, last);
}

// Distance to stop from ~kEntry m/s on a freshly built world of `surface`,
// optionally soaked. Isolated world so the entry speed is repeatable.
f32 StoppingDistance(SurfaceType surface, f32 wetness) {
  constexpr f32 kEntry = 12.0f;
  PhysicsWorld world;
  if (!world.Initialize()) return -1;
  AddFlatGround(world, surface);
  world.set_surface_wetness(wetness);
  VehicleId car = SpawnSettledCar(world, DefaultCar());
  DriveToSpeed(world, car, kEntry, 60 * 60);
  return BrakeToStop(world, car, 60 * 40);
}

}  // namespace

int main() {
  PhysicsWorld probe;
  if (!probe.Initialize()) {
    std::fprintf(stderr, "vehicle_test: physics unavailable, skipping\n");
    return 0;
  }

  // (a) Car accelerates and the automatic box shifts past 1st gear.
  {
    PhysicsWorld world;
    world.Initialize();
    AddFlatGround(world, SurfaceType::kAsphalt);
    VehicleId car = SpawnSettledCar(world, DefaultCar());
    int max_gear = 0;
    f32 max_speed = 0;
    for (int i = 0; i < 60 * 12; ++i) {
      world.DriveVehicle(car, 1.0f, 0.0f, 0.0f, 0.0f);
      world.Update(kDt);
      PhysicsWorld::VehicleState st;
      world.GetVehicleState(car, &st);
      max_gear = std::max(max_gear, st.gear);
      max_speed = std::max(max_speed, world.VehicleForwardSpeed(car));
    }
    std::fprintf(stderr, "(a) max_speed=%.2f m/s max_gear=%d\n", max_speed, max_gear);
    if (max_speed < 10.0f) return Fail("(a) car did not accelerate");
    if (max_gear < 2) return Fail("(a) automatic box did not shift past 1st");
  }

  // (b) Braking distance on ice is much longer than on asphalt.
  {
    const f32 asphalt = StoppingDistance(SurfaceType::kAsphalt, 0.0f);
    const f32 ice = StoppingDistance(SurfaceType::kIce, 0.0f);
    std::fprintf(stderr, "(b) asphalt stop=%.2f m, ice stop=%.2f m\n", asphalt, ice);
    if (asphalt <= 0 || ice <= 0) return Fail("(b) stopping distance measurement failed");
    if (ice < asphalt * 2.0f) return Fail("(b) ice braking not much longer than asphalt");
  }

  // (c) Rain wetness measurably reduces asphalt grip (longer stop).
  {
    const f32 dry = StoppingDistance(SurfaceType::kAsphalt, 0.0f);
    const f32 wet = StoppingDistance(SurfaceType::kAsphalt, 1.0f);
    std::fprintf(stderr, "(c) dry stop=%.2f m, wet stop=%.2f m (x%.2f)\n", dry, wet, wet / dry);
    if (dry <= 0 || wet <= 0) return Fail("(c) stopping distance measurement failed");
    // Grip-limited (see DefaultCar): soaked asphalt is ~0.7 of dry grip, so the
    // stop is ~1.4x longer. Assert a ratio with slack that still proves the loss.
    if (wet <= dry * 1.2f) return Fail("(c) wetness did not reduce asphalt grip");
  }

  // (d) Manual transmission holds gear until shift_up.
  {
    PhysicsWorld world;
    world.Initialize();
    AddFlatGround(world, SurfaceType::kAsphalt);
    VehicleId car = SpawnSettledCar(world, DefaultCar());
    world.SetManualTransmission(car, true);
    PhysicsWorld::VehicleInput in;
    in.throttle = 1.0f;
    // Hold throttle well past the auto shift-up rpm; gear must stay in 1st.
    int gear_before_shift = 0;
    f32 peak_rpm = 0;
    for (int i = 0; i < 60 * 6; ++i) {
      world.DriveVehicle(car, in);
      world.Update(kDt);
      PhysicsWorld::VehicleState st;
      world.GetVehicleState(car, &st);
      gear_before_shift = st.gear;
      peak_rpm = std::max(peak_rpm, st.rpm);
      if (st.gear != 1) break;
    }
    std::fprintf(stderr, "(d) held gear=%d at peak_rpm=%.0f\n", gear_before_shift, peak_rpm);
    if (gear_before_shift != 1) return Fail("(d) manual mode did not hold 1st gear");
    // A single shift_up edge advances one gear.
    PhysicsWorld::VehicleInput up = in;
    up.shift_up = true;
    world.DriveVehicle(car, up);
    world.Update(kDt);
    PhysicsWorld::VehicleState st;
    world.GetVehicleState(car, &st);
    std::fprintf(stderr, "(d) gear after shift_up=%d\n", st.gear);
    if (st.gear != 2) return Fail("(d) shift_up did not advance a gear");
  }

  // (e) Telemetry sanity over an acceleration run.
  {
    PhysicsWorld world;
    world.Initialize();
    AddFlatGround(world, SurfaceType::kAsphalt);
    PhysicsWorld::VehicleDesc desc = DefaultCar();
    VehicleId car = SpawnSettledCar(world, desc);
    const f32 redline = desc.max_rpm > 0 ? desc.max_rpm : 6000.0f;
    for (int i = 0; i < 60 * 10; ++i) {
      world.DriveVehicle(car, 1.0f, 0.0f, 0.0f, 0.0f);
      world.Update(kDt);
      PhysicsWorld::VehicleState st;
      if (!world.GetVehicleState(car, &st)) return Fail("(e) telemetry read failed");
      if (st.rpm < 0.0f || st.rpm > redline + 50.0f) return Fail("(e) rpm out of range");
      if (st.engine_load < 0.0f || st.engine_load > 1.0f) return Fail("(e) engine_load out of [0,1]");
      for (u32 w = 0; w < st.wheel_count; ++w) {
        if (st.wheels[w].suspension_compression < 0.0f ||
            st.wheels[w].suspension_compression > 1.0f) {
          return Fail("(e) suspension_compression out of [0,1]");
        }
        if (st.wheels[w].contact && st.wheels[w].surface != SurfaceType::kAsphalt) {
          return Fail("(e) wrong surface reported under wheel");
        }
      }
    }
    std::fprintf(stderr, "(e) telemetry ranges OK\n");
  }

  // (f) Aquaplaning: a fast car over a water plane loses lateral grip vs dry.
  {
    // The car spawns heading +Z; a full lock builds a lateral (world-X) path
    // offset only if the tires bite. Aquaplaning tires wash out, so the flooded
    // car plows nearly straight and the peak lateral offset collapses.
    auto lateral_offset = [](bool flooded) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      AddFlatGround(world, SurfaceType::kAsphalt);
      if (flooded) {
        world.set_water_height([](const Vec3&, f32* h, Vec3* flow) {
          *h = 0.3f;  // still water 0.3 m over the road
          *flow = Vec3{};
          return true;
        });
      }
      VehicleId car = SpawnSettledCar(world, DefaultCar());
      DriveToSpeed(world, car, 18.0f, 60 * 30);
      Vec3 start{};
      f32 rot[4];
      world.GetVehicleTransform(car, &start, rot);
      f32 peak = 0;
      for (int i = 0; i < 60 * 2; ++i) {
        world.DriveVehicle(car, 0.6f, 1.0f, 0.0f, 0.0f);
        world.Update(kDt);
        Vec3 pos;
        world.GetVehicleTransform(car, &pos, rot);
        peak = std::max(peak, std::fabs(pos.x - start.x));
      }
      return peak;
    };
    const f32 dry = lateral_offset(false);
    const f32 wet = lateral_offset(true);
    std::fprintf(stderr, "(f) dry lateral offset=%.2f m, aquaplane offset=%.2f m\n", dry, wet);
    if (dry <= wet * 1.3f) return Fail("(f) aquaplaning did not reduce cornering grip");
  }

  // (g) Standing-water drag: at fixed throttle the top speed reached through a
  // flooded straight is below the dry top speed. The flooded-wheel drag uses the
  // VEHICLE's contact-point velocity; on a static road the ground body's velocity
  // is zero, so a bug there would leave the drag inert and the two speeds equal.
  {
    auto top_speed = [](bool flooded) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      AddFlatGround(world, SurfaceType::kAsphalt);
      if (flooded) {
        world.set_water_height([](const Vec3&, f32* h, Vec3* flow) {
          *h = 0.10f;  // ~0.1 m of standing water over the whole road
          *flow = Vec3{};
          return true;
        });
      }
      VehicleId car = SpawnSettledCar(world, DefaultCar());
      f32 peak = 0;
      for (int i = 0; i < 60 * 25; ++i) {  // 25 s of full throttle down a straight
        world.DriveVehicle(car, 1.0f, 0.0f, 0.0f, 0.0f);
        world.Update(kDt);
        peak = std::max(peak, world.VehicleForwardSpeed(car));
      }
      return peak;
    };
    const f32 dry = top_speed(false);
    const f32 wet = top_speed(true);
    std::fprintf(stderr, "(g) top speed: dry=%.2f m/s standing-water=%.2f m/s (x%.2f)\n", dry, wet,
                 wet / dry);
    if (dry <= 0) return Fail("(g) dry top-speed measurement failed");
    if (wet >= dry * 0.9f) return Fail("(g) standing water did not drag top speed down");
  }

  // (h) Free-rolling chassis: an engine-disconnected vehicle (a towed trailer).
  // It must (1) step without tripping Jolt's driven-differential assertion,
  // (2) NOT accelerate on its own throttle, (3) coast forward when towed with an
  // external force, and (4) still steer its front axle.
  {
    PhysicsWorld world;
    world.Initialize();
    AddFlatGround(world, SurfaceType::kAsphalt);
    PhysicsWorld::VehicleDesc desc = DefaultCar();
    desc.free_rolling = true;
    VehicleId trailer = SpawnSettledCar(world, desc);  // steps here already (no assert)

    // (2) Full throttle does essentially nothing: the engine reaches no wheel.
    for (int i = 0; i < 60 * 3; ++i) {
      world.DriveVehicle(trailer, 1.0f, 0.0f, 0.0f, 0.0f);
      world.Update(kDt);
    }
    const f32 self_driven = world.VehicleForwardSpeed(trailer);
    std::fprintf(stderr, "(h) free-rolling self-driven speed after 3 s throttle=%.3f m/s\n",
                 self_driven);
    if (std::fabs(self_driven) > 1.0f) return Fail("(h) free-rolling vehicle drove itself");

    // (3) Tow it: a steady forward (+Z) force on the chassis body makes it roll.
    const physics::BodyId body = world.GetVehicleBody(trailer);
    if (body == 0) return Fail("(h) free-rolling vehicle has no chassis body");
    for (int i = 0; i < 60 * 4; ++i) {
      Vec3 pos{};
      f32 rot[4];
      world.GetVehicleTransform(trailer, &pos, rot);
      world.AddForceAtPoint(body, Vec3{0, 0, 6000.0f}, pos);
      world.DriveVehicle(trailer, 0.0f, 0.0f, 0.0f, 0.0f);
      world.Update(kDt);
    }
    const f32 towed = world.VehicleForwardSpeed(trailer);
    std::fprintf(stderr, "(h) free-rolling towed speed after 4 s=%.3f m/s\n", towed);
    if (towed < 2.0f) return Fail("(h) free-rolling vehicle did not roll forward when towed");

    // (4) Steering still works: hold a tow force and full lock, heading changes.
    Vec3 p0{};
    f32 r0[4];
    world.GetVehicleTransform(trailer, &p0, r0);
    const f32 start_heading = std::atan2(Rotate(Quat{r0[0], r0[1], r0[2], r0[3]}, Vec3{0, 0, 1}).x,
                                         Rotate(Quat{r0[0], r0[1], r0[2], r0[3]}, Vec3{0, 0, 1}).z);
    for (int i = 0; i < 60 * 3; ++i) {
      Vec3 pos{};
      f32 rot[4];
      world.GetVehicleTransform(trailer, &pos, rot);
      world.AddForceAtPoint(body, Vec3{0, 0, 6000.0f}, pos);
      world.DriveVehicle(trailer, 0.0f, 1.0f, 0.0f, 0.0f);
      world.Update(kDt);
    }
    Vec3 p1{};
    f32 r1[4];
    world.GetVehicleTransform(trailer, &p1, r1);
    const Vec3 fdir = Rotate(Quat{r1[0], r1[1], r1[2], r1[3]}, Vec3{0, 0, 1});
    const f32 end_heading = std::atan2(fdir.x, fdir.z);
    f32 dh = end_heading - start_heading;
    while (dh > 3.14159265f) dh -= 6.2831853f;
    while (dh < -3.14159265f) dh += 6.2831853f;
    std::fprintf(stderr, "(h) free-rolling heading change while steering=%.1f deg\n",
                 dh * 57.2958f);
    if (std::fabs(dh) < 0.05f) return Fail("(h) free-rolling front axle did not steer");
  }

  // (i) Manual transmission routes through traction control: on ice, TC-on holds
  // the driven wheels nearer their grip peak (less runaway slip) than TC-off.
  {
    auto mean_slip_on_ice = [](bool tc) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      AddFlatGround(world, SurfaceType::kIce);
      PhysicsWorld::VehicleDesc desc = DefaultCar();
      desc.traction_control = tc;
      VehicleId car = SpawnSettledCar(world, desc);
      world.SetManualTransmission(car, true);
      PhysicsWorld::VehicleInput in;
      in.throttle = 1.0f;
      f64 slip_sum = 0;
      int slip_n = 0;
      for (int i = 0; i < 60 * 10; ++i) {
        world.DriveVehicle(car, in);
        world.Update(kDt);
        // Only sample once rolling: TC (like a real system) is disengaged at
        // launch, where the slip-ratio denominator makes the reading meaningless.
        if (std::fabs(world.VehicleForwardSpeed(car)) < 5.0f) continue;
        PhysicsWorld::VehicleState st;
        if (!world.GetVehicleState(car, &st)) continue;
        f32 peak = 0;
        for (u32 w = 0; w < st.wheel_count; ++w)
          peak = std::max(peak, std::fabs(st.wheels[w].longitudinal_slip));
        slip_sum += peak;
        ++slip_n;
      }
      return slip_n > 0 ? static_cast<f32>(slip_sum / slip_n) : 0.0f;
    };
    const f32 tc_off = mean_slip_on_ice(false);
    const f32 tc_on = mean_slip_on_ice(true);
    std::fprintf(stderr, "(i) manual ice mean driven slip: TC-off=%.3f TC-on=%.3f\n", tc_off,
                 tc_on);
    if (tc_off <= 0.0f) return Fail("(i) manual ice run never got rolling to measure slip");
    if (tc_on >= tc_off * 0.9f) return Fail("(i) manual mode did not apply traction control");
  }

  // (j) Manual clutch telemetry: a disengaged clutch delivers ~no torque to the
  // wheels (though the engine still revs) and does NOT read as a gear shift.
  {
    PhysicsWorld world;
    world.Initialize();
    AddFlatGround(world, SurfaceType::kAsphalt);
    VehicleId car = SpawnSettledCar(world, DefaultCar());
    world.SetManualTransmission(car, true);
    PhysicsWorld::VehicleInput in;
    in.throttle = 1.0f;
    in.clutch = 1.0f;  // fully disengaged, held (not a shift)
    for (int i = 0; i < 60 * 2; ++i) {
      world.DriveVehicle(car, in);
      world.Update(kDt);
    }
    PhysicsWorld::VehicleState dis;
    if (!world.GetVehicleState(car, &dis)) return Fail("(j) telemetry read failed");
    std::fprintf(stderr,
                 "(j) clutch held: engine_torque=%.1f Nm rpm=%.0f is_shifting=%d load=%.3f\n",
                 dis.engine_torque, dis.rpm, dis.is_shifting ? 1 : 0, dis.engine_load);
    if (dis.engine_torque > 1.0f) return Fail("(j) disengaged clutch still delivers wheel torque");
    if (dis.is_shifting) return Fail("(j) holding the clutch wrongly reads as is_shifting");

    // Re-engage the clutch: torque now reaches the wheels.
    in.clutch = 0.0f;
    for (int i = 0; i < 30; ++i) {
      world.DriveVehicle(car, in);
      world.Update(kDt);
    }
    PhysicsWorld::VehicleState eng;
    world.GetVehicleState(car, &eng);
    std::fprintf(stderr, "(j) clutch engaged: engine_torque=%.1f Nm\n", eng.engine_torque);
    if (eng.engine_torque <= 1.0f) return Fail("(j) engaged clutch delivered no torque");
  }

  std::fprintf(stderr, "vehicle_test: all checks passed\n");
  return 0;
}
