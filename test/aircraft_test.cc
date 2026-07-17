// Headless fixed-wing checks over the public PhysicsWorld force API (no GPU):
// the plane settles on its gear, rolls out and rotates into a climb under full
// power, an over-MTOM load ruins that climb, a high-alpha low-speed state
// stalls and sinks, flaps shorten the takeoff, brakes stop a fast roll, the
// rudder yaws it on the ground and in the air, and nothing ever goes NaN,
// including deliberate post-stall tumbling. Each scenario builds its own flat
// height-field runway at y = 0.

#include <cmath>
#include <cstdio>
#include <vector>

#include "core/math.h"
#include "physics/aircraft.h"
#include "physics/physics_world.h"

using namespace rx;
using physics::Aircraft;
using physics::AircraftDesc;
using physics::AircraftInput;
using physics::AircraftState;
using physics::PhysicsWorld;
using physics::SurfaceType;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "aircraft_test FAIL: %s\n", what);
  return 1;
}

void AddRunway(PhysicsWorld& world) {
  constexpr u32 kSamples = 64;
  constexpr f32 kSize = 3000.0f;  // long enough for a full takeoff roll
  std::vector<f32> heights(static_cast<size_t>(kSamples) * kSamples, 0.0f);
  world.AddHeightField(Vec3{-kSize * 0.5f, 0.0f, -kSize * 0.5f}, heights.data(), kSamples, kSize,
                       SurfaceType::kConcrete);
}

bool IsFinite(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool StateFinite(const AircraftState& s) {
  return std::isfinite(s.airspeed_mps) && std::isfinite(s.vertical_speed_mps) &&
         std::isfinite(s.alpha_deg) && std::isfinite(s.beta_deg) && std::isfinite(s.rpm) &&
         std::isfinite(s.engine_load) && IsFinite(s.position) && std::isfinite(s.rotation.x) &&
         std::isfinite(s.rotation.y) && std::isfinite(s.rotation.z) && std::isfinite(s.rotation.w);
}

// Spawns a plane a little high and lets the suspension settle under zero input.
Aircraft* SpawnSettled(PhysicsWorld& world, const AircraftDesc& desc, Aircraft* storage) {
  Aircraft* a = new (storage) Aircraft(world, desc, Vec3{0, 1.7f, 0}, 0.0f);
  AircraftInput idle;
  for (int i = 0; i < 150; ++i) {
    a->Update(idle, kDt);
    world.Update(kDt);
  }
  return a;
}

// Heading (yaw) of the plane in radians about +Y, from its forward axis.
f32 Heading(const Aircraft& a) {
  Vec3 fwd = Rotate(a.state().rotation, Vec3{0, 0, 1});
  return std::atan2(fwd.x, fwd.z);
}

}  // namespace

int main() {
  PhysicsWorld probe;
  if (!probe.Initialize()) {
    std::fprintf(stderr, "aircraft_test: physics unavailable, skipping\n");
    return 0;
  }

  alignas(Aircraft) unsigned char storage[sizeof(Aircraft)];

  // (a) At rest on the gear: settles on the suspension, does not sink through
  // the runway, and sits still.
  {
    PhysicsWorld world;
    world.Initialize();
    AddRunway(world);
    Aircraft* a = SpawnSettled(world, AircraftDesc{}, reinterpret_cast<Aircraft*>(storage));
    AircraftInput idle;
    for (int i = 0; i < 120; ++i) {
      a->Update(idle, kDt);
      world.Update(kDt);
    }
    const AircraftState& s = a->state();
    std::fprintf(stderr, "(a) rest y=%.3f vy=%.3f speed=%.3f on_ground=%d comp=%.2f/%.2f/%.2f\n",
                 s.position.y, s.vertical_speed_mps, s.airspeed_mps, s.on_ground,
                 s.gear_compression[0], s.gear_compression[1], s.gear_compression[2]);
    if (!s.on_ground) return Fail("(a) not resting on the gear");
    if (s.position.y < 0.5f) return Fail("(a) sank through the runway");
    if (std::fabs(s.vertical_speed_mps) > 0.2f || s.airspeed_mps > 0.3f)
      return Fail("(a) not settled/stationary");
    a->~Aircraft();
  }

  // (b) Full throttle down the runway, rotate with up-elevator past rotation
  // speed, lift off and climb several m/s. Records rotation speed and the peak
  // climb rate for the report.
  f32 normal_liftoff_dist = 0;
  {
    PhysicsWorld world;
    world.Initialize();
    AddRunway(world);
    Aircraft* a = SpawnSettled(world, AircraftDesc{}, reinterpret_cast<Aircraft*>(storage));
    f32 rotate_speed = 0, liftoff_speed = 0, peak_climb = 0, max_alt = 0;
    bool airborne = false;
    const f32 ground_y = a->state().position.y;
    for (int i = 0; i < 60 * 45; ++i) {
      AircraftInput in;
      in.throttle = 1.0f;
      in.pitch = a->state().airspeed_mps > 26.0f ? 0.7f : 0.0f;  // rotate at Vr
      if (in.pitch > 0 && rotate_speed == 0) rotate_speed = a->state().airspeed_mps;
      a->Update(in, kDt);
      world.Update(kDt);
      const AircraftState& s = a->state();
      if (!StateFinite(s)) return Fail("(b) NaN during takeoff");
      const f32 alt = s.position.y - ground_y;
      max_alt = std::max(max_alt, alt);
      if (!airborne && !s.on_ground && alt > 0.5f) {
        airborne = true;
        liftoff_speed = s.airspeed_mps;
        normal_liftoff_dist = s.position.z;
      }
      if (airborne) peak_climb = std::max(peak_climb, s.vertical_speed_mps);
    }
    std::fprintf(stderr,
                 "(b) Vr=%.1f Vlof=%.1f liftoff_dist=%.0fm max_alt=%.1fm peak_climb=%.2f m/s\n",
                 rotate_speed, liftoff_speed, normal_liftoff_dist, max_alt, peak_climb);
    if (max_alt < 8.0f) return Fail("(b) did not lift off / gain altitude");
    if (peak_climb < 2.0f) return Fail("(b) climb rate too weak");
    a->~Aircraft();
  }

  // (c) Same airframe, payload pushed over MTOM: markedly longer ground roll
  // (distance to reach a reference speed) and no sustained climb, with the
  // identical inputs and time budget as the light run in (b).
  {
    // Returns {distance to reach ref_speed, max altitude gain, climb rate near
    // the end of the window}. Identical control law to (b).
    auto takeoff_run = [&](f32 payload, f32 ref_speed, f32* roll_dist, f32* max_alt,
                           f32* late_vs) -> bool {
      PhysicsWorld world;
      world.Initialize();
      AddRunway(world);
      AircraftDesc desc;
      desc.payload_kg = payload;
      Aircraft* a = SpawnSettled(world, desc, reinterpret_cast<Aircraft*>(storage));
      const bool over = a->over_mtom();
      const f32 ground_y = a->state().position.y;
      const f32 start_z = a->state().position.z;
      *roll_dist = -1;
      *max_alt = 0;
      *late_vs = 0;
      for (int i = 0; i < 60 * 45; ++i) {
        AircraftInput in;
        in.throttle = 1.0f;
        in.pitch = a->state().airspeed_mps > 26.0f ? 0.7f : 0.0f;
        a->Update(in, kDt);
        world.Update(kDt);
        const AircraftState& s = a->state();
        if (!StateFinite(s)) return false;
        if (*roll_dist < 0 && s.airspeed_mps >= ref_speed) *roll_dist = s.position.z - start_z;
        *max_alt = std::max(*max_alt, s.position.y - ground_y);
        if (i > 60 * 43) *late_vs = s.vertical_speed_mps;
      }
      a->~Aircraft();
      return over;
    };

    constexpr f32 kRef = 24.0f;  // below the light-plane rotation speed
    f32 light_roll, light_alt, light_vs;
    f32 heavy_roll, heavy_alt, heavy_vs;
    takeoff_run(220.0f, kRef, &light_roll, &light_alt, &light_vs);
    const bool over = takeoff_run(470.0f, kRef, &heavy_roll, &heavy_alt, &heavy_vs);
    std::fprintf(stderr,
                 "(c) light: roll_to_%.0f=%.0fm alt=%.1f vs=%.2f | heavy(over_mtom=%d): "
                 "roll=%.0fm alt=%.1f vs=%.2f\n",
                 kRef, light_roll, light_alt, light_vs, over, heavy_roll, heavy_alt, heavy_vs);
    if (!over) return Fail("(c) test load is not over MTOM");
    if (light_roll <= 0 || heavy_roll <= 0) return Fail("(c) never reached reference speed");
    if (heavy_roll < light_roll * 1.3f) return Fail("(c) overloaded ground roll not markedly longer");
    // Overloaded fails to sustain a climb the light plane easily makes.
    if (heavy_vs > 0.5f || heavy_alt > light_alt * 0.5f)
      return Fail("(c) overloaded plane still climbed");
  }

  // (d) Stall: trimmed slow with full up-elevator held, the wing passes stall
  // and the plane sinks despite the elevator.
  {
    PhysicsWorld world;
    world.Initialize();
    AddRunway(world);
    // Start airborne and slow: spawn high, low speed, full up elevator, idle
    // power so it cannot accelerate out of the stall.
    Aircraft* a = new (storage) Aircraft(world, AircraftDesc{}, Vec3{0, 200.0f, 0}, 0.0f);
    f32 min_vs = 0;
    bool saw_stall = false;
    for (int i = 0; i < 60 * 12; ++i) {
      AircraftInput in;
      in.throttle = 0.0f;
      in.pitch = 1.0f;  // hold the nose up into the stall
      a->Update(in, kDt);
      world.Update(kDt);
      const AircraftState& s = a->state();
      if (!StateFinite(s)) return Fail("(d) NaN during stall");
      if (s.stalled_left || s.stalled_right) saw_stall = true;
      min_vs = std::min(min_vs, s.vertical_speed_mps);
    }
    const AircraftState& s = a->state();
    std::fprintf(stderr, "(d) stalled_l=%d stalled_r=%d alpha=%.1f min_vs=%.1f end_vs=%.1f\n",
                 s.stalled_left, s.stalled_right, s.alpha_deg, min_vs, s.vertical_speed_mps);
    if (!saw_stall) return Fail("(d) wing never stalled");
    if (min_vs > -3.0f) return Fail("(d) stalled wing did not sink");
    a->~Aircraft();
  }

  // (e) Flaps shorten the takeoff: liftoff happens at a shorter ground distance
  // with full flaps than clean, all else equal.
  {
    auto liftoff_distance = [&](f32 flaps) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      AddRunway(world);
      Aircraft* a = SpawnSettled(world, AircraftDesc{}, reinterpret_cast<Aircraft*>(storage));
      const f32 ground_y = a->state().position.y;
      f32 dist = -1;
      for (int i = 0; i < 60 * 45; ++i) {
        AircraftInput in;
        in.throttle = 1.0f;
        in.flaps = flaps;
        in.pitch = a->state().airspeed_mps > 24.0f ? 0.7f : 0.0f;
        a->Update(in, kDt);
        world.Update(kDt);
        const AircraftState& s = a->state();
        if (!s.on_ground && s.position.y - ground_y > 0.5f) {
          dist = s.position.z;
          break;
        }
      }
      a->~Aircraft();
      return dist;
    };
    const f32 clean = liftoff_distance(0.0f);
    const f32 flapped = liftoff_distance(1.0f);
    std::fprintf(stderr, "(e) clean liftoff=%.0fm  full-flap liftoff=%.0fm\n", clean, flapped);
    if (clean <= 0 || flapped <= 0) return Fail("(e) a config never lifted off");
    if (flapped >= clean * 0.95f) return Fail("(e) flaps did not shorten the takeoff");
  }

  // (f) Brakes stop a fast ground roll in far less distance than coasting.
  {
    auto roll_distance = [&](bool brake) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      AddRunway(world);
      Aircraft* a = SpawnSettled(world, AircraftDesc{}, reinterpret_cast<Aircraft*>(storage));
      // Accelerate to ~22 m/s (below Vr, gear stays down), no rotation.
      for (int i = 0; i < 60 * 40 && a->state().airspeed_mps < 22.0f; ++i) {
        AircraftInput in;
        in.throttle = 1.0f;
        a->Update(in, kDt);
        world.Update(kDt);
      }
      const f32 start_z = a->state().position.z;
      f32 stop_z = start_z;
      for (int i = 0; i < 60 * 30; ++i) {
        AircraftInput in;
        in.throttle = 0.0f;
        in.brakes = brake ? 1.0f : 0.0f;
        a->Update(in, kDt);
        world.Update(kDt);
        stop_z = a->state().position.z;
        if (a->state().airspeed_mps < 0.6f) break;
      }
      a->~Aircraft();
      return stop_z - start_z;
    };
    const f32 coast = roll_distance(false);
    const f32 braked = roll_distance(true);
    std::fprintf(stderr, "(f) coast dist=%.0fm  braked dist=%.0fm\n", coast, braked);
    if (braked >= coast * 0.6f) return Fail("(f) brakes did not stop it much shorter");
  }

  // (g) Rudder yaws the plane: on the ground (nose-wheel steer) and in the air
  // (fin side force turns the heading).
  {
    // Ground: from rest, roll slowly with full rudder; the heading swings.
    PhysicsWorld world;
    world.Initialize();
    AddRunway(world);
    Aircraft* a = SpawnSettled(world, AircraftDesc{}, reinterpret_cast<Aircraft*>(storage));
    const f32 h0 = Heading(*a);
    for (int i = 0; i < 60 * 8; ++i) {
      AircraftInput in;
      in.throttle = 0.35f;
      in.yaw = 1.0f;
      a->Update(in, kDt);
      world.Update(kDt);
      if (!StateFinite(a->state())) return Fail("(g) NaN during ground steer");
    }
    const f32 ground_yaw = std::fabs(Heading(*a) - h0);
    a->~Aircraft();

    // Air: trim in level fast flight, apply full rudder, measure heading change.
    PhysicsWorld air;
    air.Initialize();
    AddRunway(air);
    Aircraft* b = new (storage) Aircraft(air, AircraftDesc{}, Vec3{0, 300.0f, 0}, 0.0f);
    // Establish ~45 m/s of forward speed with power before the rudder input.
    for (int i = 0; i < 60 * 6; ++i) {
      AircraftInput in;
      in.throttle = 1.0f;
      in.pitch = 0.05f;  // hold roughly level
      b->Update(in, kDt);
      air.Update(kDt);
    }
    const f32 ah0 = Heading(*b);
    for (int i = 0; i < 60 * 4; ++i) {
      AircraftInput in;
      in.throttle = 1.0f;
      in.pitch = 0.05f;
      in.yaw = 1.0f;
      b->Update(in, kDt);
      air.Update(kDt);
      if (!StateFinite(b->state())) return Fail("(g) NaN during air yaw");
    }
    const f32 air_yaw = std::fabs(Heading(*b) - ah0);
    std::fprintf(stderr, "(g) ground yaw=%.1f deg  air yaw=%.1f deg\n", ground_yaw * 57.2958f,
                 air_yaw * 57.2958f);
    b->~Aircraft();
    if (ground_yaw < 0.15f) return Fail("(g) rudder/nose-wheel did not steer on the ground");
    if (air_yaw < 0.05f) return Fail("(g) rudder did not yaw in flight");
  }

  // (h) No NaNs after minutes of abusive sim: full deflections, throttle
  // chopping, deliberate post-stall tumbling.
  {
    PhysicsWorld world;
    world.Initialize();
    AddRunway(world);
    Aircraft* a = new (storage) Aircraft(world, AircraftDesc{}, Vec3{0, 500.0f, 0}, 0.3f);
    for (int i = 0; i < 60 * 180; ++i) {
      AircraftInput in;
      in.throttle = (i / 30) % 2 ? 1.0f : 0.0f;
      in.pitch = std::sin(i * 0.05f);
      in.roll = std::sin(i * 0.11f);
      in.yaw = std::cos(i * 0.07f);
      in.flaps = (i / 120) % 2 ? 1.0f : 0.0f;
      in.brakes = (i % 90) < 10 ? 1.0f : 0.0f;
      a->Update(in, kDt);
      world.Update(kDt);
      if (!StateFinite(a->state())) return Fail("(h) NaN during tumbling abuse");
    }
    std::fprintf(stderr, "(h) survived 180s of tumbling, y=%.1f speed=%.1f\n", a->state().position.y,
                 a->state().airspeed_mps);
    a->~Aircraft();
  }

  // (i) Headwind shortens the ground roll: with the airmass moving toward the
  // plane it reaches flying airspeed at a lower ground speed, so it leaves the
  // ground in a shorter distance than in calm air. Settle calm, then set the
  // world wind and roll.
  {
    auto ground_roll = [&](f32 headwind) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      AddRunway(world);
      Aircraft* a = SpawnSettled(world, AircraftDesc{}, reinterpret_cast<Aircraft*>(storage));
      world.set_wind(Vec3{0, 0, -headwind});  // blowing toward -Z, opposing +Z travel
      const f32 ground_y = a->state().position.y;
      const f32 start_z = a->state().position.z;
      f32 dist = -1;
      for (int i = 0; i < 60 * 45; ++i) {
        AircraftInput in;
        in.throttle = 1.0f;
        in.pitch = a->state().airspeed_mps > 26.0f ? 0.7f : 0.0f;
        a->Update(in, kDt);
        world.Update(kDt);
        const AircraftState& s = a->state();
        if (!StateFinite(s)) return -2;
        if (!s.on_ground && s.position.y - ground_y > 0.5f) {
          dist = s.position.z - start_z;
          break;
        }
      }
      a->~Aircraft();
      return dist;
    };
    const f32 calm = ground_roll(0.0f);
    const f32 head = ground_roll(10.0f);
    std::fprintf(stderr, "(i) calm roll=%.0fm  headwind(10 m/s) roll=%.0fm\n", calm, head);
    if (calm <= 0 || head <= 0) return Fail("(i) a config never lifted off");
    if (head >= calm * 0.85f) return Fail("(i) headwind did not shorten the ground roll");
  }

  // (ii) A steady crosswind in flight weathervanes the nose: the aero is
  // measured relative to the airmass, so the crosswind is felt as sideslip and
  // the fin yaws the nose toward the relative wind - a heading change a
  // calm-air plane holding identical controls does not develop. (dx, the
  // lateral ground drift, is reported for information; over this short window
  // the nose swings rather than the track translating far.)
  {
    auto fly = [&](f32 crosswind, f32* dx, f32* dyaw) {
      PhysicsWorld world;
      world.Initialize();
      AddRunway(world);
      Aircraft* a = new (storage) Aircraft(world, AircraftDesc{}, Vec3{0, 300.0f, 0}, 0.0f);
      for (int i = 0; i < 60 * 6; ++i) {  // establish level flight
        AircraftInput in;
        in.throttle = 1.0f;
        in.pitch = 0.05f;
        a->Update(in, kDt);
        world.Update(kDt);
      }
      const f32 x0 = a->state().position.x;
      const f32 h0 = Heading(*a);
      world.set_wind(Vec3{crosswind, 0, 0});  // steady wind toward +X
      for (int i = 0; i < 90; ++i) {  // 1.5 s: the initial weathervane response
        AircraftInput in;
        in.throttle = 1.0f;
        in.pitch = 0.05f;
        a->Update(in, kDt);
        world.Update(kDt);
        if (!StateFinite(a->state())) return Fail("(ii) NaN in crosswind");
      }
      *dx = a->state().position.x - x0;
      *dyaw = Heading(*a) - h0;
      a->~Aircraft();
      return 0;
    };
    f32 calm_dx = 0, calm_yaw = 0, cross_dx = 0, cross_yaw = 0;
    if (fly(0.0f, &calm_dx, &calm_yaw)) return 1;
    if (fly(12.0f, &cross_dx, &cross_yaw)) return 1;
    std::fprintf(stderr, "(ii) calm dx=%.1f yaw=%.2f | crosswind dx=%.1f yaw=%.2f deg\n", calm_dx,
                 calm_yaw * 57.2958f, cross_dx, cross_yaw * 57.2958f);
    if (std::fabs(cross_yaw) < 0.15f)
      return Fail("(ii) crosswind did not weathervane the nose");
    if (std::fabs(cross_yaw) <= std::fabs(calm_yaw) + 0.1f)
      return Fail("(ii) crosswind weathervane not distinct from calm");
  }

  std::fprintf(stderr, "aircraft_test: all checks passed\n");
  return 0;
}
