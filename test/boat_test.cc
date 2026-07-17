// Headless boat-realism checks over the force-based hull model (no GPU): a
// flat water plane is installed via set_water_height and the boat is driven per
// fixed step (Boat::Update BEFORE PhysicsWorld::Update). Scenarios: floating
// draft + idle drift, acceleration, steering, drag decay, planing vs a
// no-planing hull, river drift, self-righting from a knock-down, buoyancy
// exemption (no double buoyancy), and NaN-free simulation on Gerstner chop.

#include <cmath>
#include <cstdio>

#include "core/math.h"
#include "physics/boat.h"
#include "physics/physics_world.h"
#include "physics/water_waves.h"

using namespace rx;
using physics::Boat;
using physics::BoatDesc;
using physics::BoatInput;
using physics::BoatState;
using physics::PhysicsWorld;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "boat_test FAIL: %s\n", what);
  return 1;
}

// Flat still-water plane at y = 0, no flow.
void InstallFlatWater(PhysicsWorld& world) {
  world.set_water_height([](const Vec3&, f32* h, Vec3* flow) {
    *h = 0.0f;
    *flow = Vec3{};
    return true;
  });
}

// Advances the coupled boat + world one fixed step (correct ordering).
void Step(PhysicsWorld& world, Boat& boat, const BoatInput& in) {
  boat.Update(in, kDt);
  world.Update(kDt);
}

// Runs `steps` steps with a constant input; returns the final state.
BoatState Run(PhysicsWorld& world, Boat& boat, const BoatInput& in, int steps) {
  for (int i = 0; i < steps; ++i) Step(world, boat, in);
  return boat.state();
}

// Heading angle (radians) of the hull forward axis in the XZ plane.
f32 Heading(const BoatState& s) {
  const Vec3 fwd = Rotate(s.rotation, Vec3{0, 0, 1});
  return std::atan2(fwd.x, fwd.z);
}

// World up projected onto the hull's up axis: 1 = upright, 0 = on its side.
f32 Uprightness(const BoatState& s) {
  const Vec3 up = Rotate(s.rotation, Vec3{0, 1, 0});
  return up.y;
}

}  // namespace

int main() {
  PhysicsWorld probe;
  if (!probe.Initialize()) {
    std::fprintf(stderr, "boat_test: physics unavailable, skipping\n");
    return 0;
  }

  // (a) Settles floating with a sane draft and stays put with no input.
  {
    PhysicsWorld world;
    world.Initialize();
    InstallFlatWater(world);
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.5f, 0}, 0.0f);
    if (!boat.valid()) return Fail("(a) boat spawn failed");
    Run(world, boat, {}, 60 * 5);  // let it settle
    const BoatState s0 = boat.state();
    const f32 hh_y = desc.hull_half_extent.y;
    const f32 bottom = s0.position.y - hh_y;  // hull bottom (water at y=0)
    const f32 deck = s0.position.y + hh_y;     // hull top
    const f32 draft = -bottom;                 // submerged depth of the bottom
    std::fprintf(stderr, "(a) center_y=%.3f draft=%.3f deck=%.3f wetted=%.2f\n",
                 s0.position.y, draft, deck, s0.wetted);
    if (draft <= 0.02f || draft > 0.45f) return Fail("(a) draft not in a sane floating range");
    if (deck <= 0.05f) return Fail("(a) deck not above water");
    if (s0.wetted <= 0.0f) return Fail("(a) no hull samples submerged");
    // Drift over 3 more seconds with no input must be tiny.
    const Vec3 p0 = s0.position;
    Run(world, boat, {}, 60 * 3);
    const Vec3 p1 = boat.state().position;
    const f32 drift = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.z - p0.z) * (p1.z - p0.z));
    std::fprintf(stderr, "(a) idle drift=%.4f m\n", drift);
    if (drift > 0.15f) return Fail("(a) idle boat drifted");
  }

  // (b) Full throttle accelerates it forward (+Z at yaw 0) past several m/s.
  {
    PhysicsWorld world;
    world.Initialize();
    InstallFlatWater(world);
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.4f, 0}, 0.0f);
    Run(world, boat, {}, 60 * 2);  // settle
    BoatInput in;
    in.throttle = 1.0f;
    const BoatState s = Run(world, boat, in, 60 * 15);
    std::fprintf(stderr, "(b) forward_speed=%.2f pos.z=%.2f pos.x=%.2f up=%.2f\n", s.forward_speed,
                 s.position.z, s.position.x, Uprightness(s));
    if (s.forward_speed < 4.0f) return Fail("(b) boat did not accelerate to several m/s");
    if (s.position.z < 10.0f) return Fail("(b) boat did not travel forward (+Z)");
    if (std::fabs(s.position.x) > 3.0f) return Fail("(b) boat veered sideways under straight throttle");
  }

  // (c) Sustained steer while moving yaws the boat.
  {
    PhysicsWorld world;
    world.Initialize();
    InstallFlatWater(world);
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.4f, 0}, 0.0f);
    Run(world, boat, {}, 60 * 2);
    BoatInput fwd;
    fwd.throttle = 1.0f;
    Run(world, boat, fwd, 60 * 6);  // get moving
    const f32 h0 = Heading(boat.state());
    BoatInput turn;
    turn.throttle = 1.0f;
    turn.steer = 1.0f;
    const BoatState s = Run(world, boat, turn, 60 * 5);
    const f32 dh = std::fabs(Heading(s) - h0);
    std::fprintf(stderr, "(c) heading change=%.3f rad\n", dh);
    if (dh < 0.2f) return Fail("(c) sustained steer did not yaw the boat");
  }

  // (d) Cutting throttle decays speed (drag).
  {
    PhysicsWorld world;
    world.Initialize();
    InstallFlatWater(world);
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.4f, 0}, 0.0f);
    Run(world, boat, {}, 60 * 2);
    BoatInput in;
    in.throttle = 1.0f;
    const f32 top = Run(world, boat, in, 60 * 15).forward_speed;
    const f32 coast = Run(world, boat, {}, 60 * 8).forward_speed;  // throttle cut
    std::fprintf(stderr, "(d) top=%.2f coast=%.2f\n", top, coast);
    if (coast >= top * 0.6f) return Fail("(d) speed did not decay after cutting throttle");
  }

  // (e) Planing fraction rises at speed, and a planing hull tops out faster
  //     than an otherwise-identical hull with planing disabled.
  {
    auto top_speed = [](bool planing) -> BoatState {
      PhysicsWorld world;
      world.Initialize();
      InstallFlatWater(world);
      BoatDesc desc;
      if (!planing) {
        desc.plane_lift = 0.0f;
        desc.plane_drag_reduction = 0.0f;
      }
      Boat boat(world, desc, Vec3{0, 0.4f, 0}, 0.0f);
      BoatInput in;
      in.throttle = 1.0f;
      return Run(world, boat, in, 60 * 40);
    };
    const BoatState planing = top_speed(true);
    const BoatState displacement = top_speed(false);
    std::fprintf(stderr, "(e) planing top=%.2f (frac=%.2f wetted=%.2f) displacement top=%.2f\n",
                 planing.forward_speed, planing.planing, planing.wetted, displacement.forward_speed);
    if (planing.planing < 0.5f) return Fail("(e) planing fraction did not rise at speed");
    if (planing.forward_speed <= displacement.forward_speed * 1.1f) {
      return Fail("(e) planing hull did not exceed the no-planing hull top speed");
    }
  }

  // (f) River flow carries an idle boat downstream.
  {
    PhysicsWorld world;
    world.Initialize();
    world.set_water_height([](const Vec3&, f32* h, Vec3* flow) {
      *h = 0.0f;
      *flow = Vec3{2.0f, 0.0f, 0.0f};  // 2 m/s current toward +X
      return true;
    });
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.4f, 0}, 0.0f);
    Run(world, boat, {}, 60 * 2);
    const f32 x0 = boat.state().position.x;
    const BoatState s = Run(world, boat, {}, 60 * 8);
    const f32 dx = s.position.x - x0;
    std::fprintf(stderr, "(f) downstream drift dx=%.2f m\n", dx);
    if (dx < 2.0f) return Fail("(f) river flow did not carry the idle boat downstream");
  }

  // (g) Rolled to ~90 deg, the boat self-rights within a few seconds.
  {
    PhysicsWorld world;
    world.Initialize();
    InstallFlatWater(world);
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.4f, 0}, 0.0f);
    // Knock it down: roll ~80 deg about the forward (+Z) axis, then let go.
    const f32 heel = 1.4f;
    const f32 rot[4] = {0.0f, 0.0f, std::sin(heel * 0.5f), std::cos(heel * 0.5f)};
    world.SetBodyPosition(boat.body(), Vec3{0, 0.4f, 0}, rot);
    Step(world, boat, {});  // refresh telemetry from the knocked-down pose
    const f32 before = Uprightness(boat.state());
    const BoatState s = Run(world, boat, {}, 60 * 4);
    std::fprintf(stderr, "(g) uprightness before=%.2f after=%.2f\n", before, Uprightness(s));
    if (Uprightness(s) < 0.85f) return Fail("(g) boat did not self-right from a knock-down");
  }

  // (h) Buoyancy exemption: the draft matches the single-model displacement
  //     prediction, so the generic world buoyancy is not double-applied.
  {
    PhysicsWorld world;
    world.Initialize();
    InstallFlatWater(world);
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.5f, 0}, 0.0f);
    Run(world, boat, {}, 60 * 6);
    const BoatState s = boat.state();
    const f32 draft = desc.hull_half_extent.y - s.position.y;
    // Predicted equilibrium draft d = mass / (rho * footprint).
    const f32 footprint = (2.0f * desc.hull_half_extent.x) * (2.0f * desc.hull_half_extent.z);
    const f32 predicted = desc.mass / (1000.0f * footprint);
    std::fprintf(stderr, "(h) draft=%.3f predicted=%.3f\n", draft, predicted);
    if (std::fabs(draft - predicted) > 0.06f) {
      return Fail("(h) draft does not match single-model displacement (double buoyancy?)");
    }
  }

  // (i) No NaNs after a minute of Gerstner chop.
  {
    PhysicsWorld world;
    world.Initialize();
    static f32 t = 0.0f;
    t = 0.0f;
    world.set_water_height([](const Vec3& p, f32* h, Vec3* flow) {
      Vec3 f{};
      *h = physics::GerstnerWaveHeight(p.x, p.z, t, &f, nullptr);
      *flow = f;
      return true;
    });
    BoatDesc desc;
    Boat boat(world, desc, Vec3{0, 0.6f, 0}, 0.0f);
    BoatInput in;
    for (int i = 0; i < 60 * 60; ++i) {
      // Wander the helm so thrust, planing and rudder all exercise on the chop.
      in.throttle = std::sin(t * 0.7f);
      in.steer = std::sin(t * 0.3f);
      t += kDt;
      Step(world, boat, in);
      const BoatState s = boat.state();
      if (!std::isfinite(s.position.x) || !std::isfinite(s.position.y) ||
          !std::isfinite(s.position.z) || !std::isfinite(s.rpm) ||
          !std::isfinite(s.forward_speed) || !std::isfinite(s.rotation.w)) {
        return Fail("(i) NaN/Inf in boat state on chop");
      }
    }
    std::fprintf(stderr, "(i) survived 60 s of chop, final speed=%.2f rpm=%.0f\n",
                 boat.state().speed_mps, boat.state().rpm);
  }

  // (j) A strong beam wind pushes a drifting (idle) boat downwind, where in
  // calm air it barely moves. (Uprightness is printed for information: the wind
  // heels the hull only slightly - the buoyancy grid + ballast righting is
  // stiff - so it is not asserted.)
  {
    auto drift = [](f32 wind_x, f32* dx, f32* up) {
      PhysicsWorld world;
      world.Initialize();
      InstallFlatWater(world);
      BoatDesc desc;
      Boat boat(world, desc, Vec3{0, 0.5f, 0}, 0.0f);
      Run(world, boat, {}, 60 * 3);        // settle
      world.set_wind(Vec3{wind_x, 0, 0});  // beam wind toward +X (hull faces +Z)
      const f32 x0 = boat.state().position.x;
      const BoatState s = Run(world, boat, {}, 60 * 8);
      *dx = s.position.x - x0;
      *up = Uprightness(s);
    };
    f32 calm_dx = 0, calm_up = 0, wind_dx = 0, wind_up = 0;
    drift(0.0f, &calm_dx, &calm_up);
    drift(25.0f, &wind_dx, &wind_up);
    std::fprintf(stderr, "(j) calm dx=%.2f up=%.4f | beam wind(25 m/s) dx=%.2f up=%.4f\n", calm_dx,
                 calm_up, wind_dx, wind_up);
    if (wind_dx < 1.5f) return Fail("(j) beam wind did not push the boat downwind");
    if (wind_dx <= std::fabs(calm_dx) + 1.0f) return Fail("(j) wind drift not distinct from calm");
  }

  std::fprintf(stderr, "boat_test: all checks passed\n");
  return 0;
}
