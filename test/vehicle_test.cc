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
    std::fprintf(stderr, "(c) dry stop=%.2f m, wet stop=%.2f m\n", dry, wet);
    if (dry <= 0 || wet <= 0) return Fail("(c) stopping distance measurement failed");
    if (wet <= dry * 1.1f) return Fail("(c) wetness did not reduce asphalt grip");
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

  std::fprintf(stderr, "vehicle_test: all checks passed\n");
  return 0;
}
