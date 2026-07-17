// Headless proof of the force-based tethered kite (engine/physics/kite.{h,cc}).
// Needs the Jolt-enabled physics build (like boat_test/aircraft_test). Each
// lettered scenario prints its measured numbers to stderr and returns Fail(...)
// on the first bad assert.
//
//   (a) 8 m/s wind: launches and holds a stable overhead equilibrium 30 s.
//   (b) zero wind:  descends and settles low.
//   (c) steer left vs right displaces the kite laterally in opposite directions.
//   (d) reel-in shortens the line and the altitude follows it down.
//   (e) anchor towed at 8 m/s in calm air flies the kite on apparent wind.
//   (f) 60 s of gusting wind is NaN-free and never exceeds the tension cap.

#include <cmath>
#include <cstdio>

#include "core/math.h"
#include "physics/kite.h"
#include "physics/physics_world.h"

using rx::f32;
using rx::Vec3;
using rx::physics::Kite;
using rx::physics::KiteDesc;
using rx::physics::KiteInput;
using rx::physics::KiteState;
using rx::physics::PhysicsWorld;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "kite_test FAIL: %s\n", what);
  return 1;
}

// Flat ground at y = 0 so a launching kite has something to lift off from and a
// dead-air kite something to settle on.
void InstallGround(PhysicsWorld& world) { world.AddStaticBox({0, -0.5f, 0}, {200, 0.5f, 200}); }

// One coupled step: stage the kite forces, then advance the world (kite Update
// BEFORE PhysicsWorld::Update, per the contract).
void Step(PhysicsWorld& world, Kite& kite, const KiteInput& in) {
  kite.Update(in, kDt);
  world.Update(kDt);
}

KiteState Run(PhysicsWorld& world, Kite& kite, const KiteInput& in, int steps) {
  for (int i = 0; i < steps; ++i) Step(world, kite, in);
  return kite.state();
}

}  // namespace

int main() {
  {
    PhysicsWorld probe;
    if (!probe.Initialize()) {
      std::fprintf(stderr, "kite_test: physics stub, skipping\n");
      return 0;  // physics compiled as a stub: nothing to exercise
    }
  }

  const KiteDesc desc;  // ~1.5 m sport kite defaults
  const f32 line = desc.line_length_m;
  const Vec3 anchor{0, 0, 0};  // post at the origin, on the ground

  // (a) 8 m/s wind: launch and hold a stable overhead equilibrium ---------------
  {
    PhysicsWorld world;
    if (!world.Initialize()) return Fail("(a) init");
    InstallGround(world);
    world.set_wind({0, 0, 8});  // steady 8 m/s along +Z
    // Spawn on the ground a little downwind of the post, leading edge up.
    Kite kite(world, desc, anchor, Vec3{0, 0.6f, line * 0.95f}, 0.0f);
    if (!kite.valid()) return Fail("(a) spawn");

    // Give it a few seconds to launch and climb into equilibrium.
    Run(world, kite, {}, 60 * 8);
    const KiteState s = kite.state();
    std::fprintf(stderr,
                 "(a) equilibrium: altitude=%.2f m  tension=%.1f N  airspeed=%.2f m/s  "
                 "alpha=%.1f deg  taut=%d  line=%.1f m\n",
                 s.altitude_m, s.tension_n, s.airspeed_mps, s.alpha_deg, s.taut, s.line_length_m);

    // Overhead: well above half the line length, and taut.
    if (s.altitude_m < line * 0.5f) return Fail("(a) kite did not climb overhead");
    if (!s.taut) return Fail("(a) line not taut in steady wind");
    // Sane tension band: enough to carry the ~3 N of weight, far under the cap.
    if (s.tension_n < 1.0f || s.tension_n > desc.tether_max_tension * 0.5f)
      return Fail("(a) tension out of band");

    // Stays airborne and steady for another 30 s (sample the altitude spread).
    f32 lo = s.altitude_m, hi = s.altitude_m;
    for (int i = 0; i < 60 * 30; ++i) {
      Step(world, kite, {});
      const f32 a = kite.state().altitude_m;
      if (!std::isfinite(a)) return Fail("(a) NaN altitude");
      lo = std::min(lo, a);
      hi = std::max(hi, a);
    }
    std::fprintf(stderr, "(a) 30 s hover band: [%.2f, %.2f] m\n", lo, hi);
    if (lo < line * 0.4f) return Fail("(a) kite fell out of the sky during hover");
  }

  // (b) zero wind: descend and settle ------------------------------------------
  {
    PhysicsWorld world;
    if (!world.Initialize()) return Fail("(b) init");
    InstallGround(world);
    world.set_wind({0, 0, 8});
    Kite kite(world, desc, anchor, Vec3{0, 0.6f, line * 0.95f}, 0.0f);
    if (!kite.valid()) return Fail("(b) spawn");
    const f32 flying = Run(world, kite, {}, 60 * 8).altitude_m;  // aloft first
    world.set_wind({0, 0, 0});                                    // wind dies
    const f32 settled = Run(world, kite, {}, 60 * 20).altitude_m;
    std::fprintf(stderr, "(b) altitude flying=%.2f m -> settled=%.2f m\n", flying, settled);
    if (settled >= flying - 5.0f) return Fail("(b) kite did not descend when the wind died");
    // Damped by its own form drag, a lift-less kite sinks slowly and settles near
    // the ground (it hangs/rests low, not pinned to y=0).
    if (settled > 3.0f) return Fail("(b) kite did not settle low");
  }

  // (c) steer left vs right displaces the kite in opposite directions ----------
  {
    auto steered_x = [&](f32 steer) -> f32 {
      PhysicsWorld world;
      world.Initialize();
      InstallGround(world);
      world.set_wind({0, 0, 8});
      Kite kite(world, desc, anchor, Vec3{0, 0.6f, line * 0.95f}, 0.0f);
      Run(world, kite, {}, 60 * 8);  // settle overhead
      KiteInput in;
      in.steer = steer;
      // A short steer pulse, then read the lateral (X) displacement.
      const f32 x0 = kite.state().position.x;
      Run(world, kite, in, 60 * 2);
      return kite.state().position.x - x0;
    };
    const f32 left = steered_x(-1.0f);
    const f32 right = steered_x(+1.0f);
    std::fprintf(stderr, "(c) steer dX  left=%.2f m  right=%.2f m\n", left, right);
    if (std::fabs(left) < 0.5f || std::fabs(right) < 0.5f)
      return Fail("(c) steering produced no lateral displacement");
    if ((left > 0) == (right > 0)) return Fail("(c) left/right steer did not oppose");
  }

  // (d) reel-in shortens the line and the altitude follows ---------------------
  {
    PhysicsWorld world;
    if (!world.Initialize()) return Fail("(d) init");
    InstallGround(world);
    world.set_wind({0, 0, 8});
    Kite kite(world, desc, anchor, Vec3{0, 0.6f, line * 0.95f}, 0.0f);
    Run(world, kite, {}, 60 * 8);
    const f32 line0 = kite.line_length();
    const f32 alt0 = kite.state().altitude_m;
    KiteInput in;
    in.reel = -1.0f;  // reel in
    // Track the whole reel-in so a violent yank (would-be underground fling)
    // is caught, not just the endpoint.
    f32 min_alt = alt0;
    for (int i = 0; i < 60 * 5; ++i) {
      Step(world, kite, in);
      min_alt = std::min(min_alt, kite.state().altitude_m);
    }
    const f32 line1 = kite.line_length();
    const f32 alt1 = kite.state().altitude_m;
    std::fprintf(stderr, "(d) reel-in line %.1f -> %.1f m   altitude %.2f -> %.2f m (min %.2f)\n",
                 line0, line1, alt0, alt1, min_alt);
    if (line1 >= line0 - 1.0f) return Fail("(d) line did not shorten");
    if (alt1 >= alt0 - 0.5f) return Fail("(d) altitude did not follow the shorter line down");
    // Sane: the kite stays aloft and never gets flung (no underground yank).
    if (alt1 < 1.0f || min_alt < 0.5f) return Fail("(d) reel-in yanked the kite out of the sky");
  }

  // (e) anchor towed at 8 m/s in calm air flies the kite (apparent wind) --------
  {
    PhysicsWorld world;
    if (!world.Initialize()) return Fail("(e) init");
    InstallGround(world);
    world.set_wind({0, 0, 0});  // dead calm; motion makes the wind
    Vec3 anc{0, 0.5f, 0};
    // Kite spawned near-taut on the downwind (+Z) side; towing the anchor along
    // -Z drags the kite -Z so it feels a +Z apparent wind (the same the launch
    // attitude presents its belly to) - a kitesurf tow in still air.
    Kite kite(world, desc, anchor, Vec3{0, 0.6f, line * 0.95f}, 0.0f);
    if (!kite.valid()) return Fail("(e) spawn");
    KiteInput in;
    f32 max_alt = 0.0f;
    // Tow the anchor along -Z at 8 m/s for 15 s (kitesurf-style).
    for (int i = 0; i < 60 * 15; ++i) {
      anc.z -= 8.0f * kDt;
      kite.set_anchor(anc);
      Step(world, kite, in);
      const f32 a = kite.state().altitude_m;  // height above the (moving) anchor
      if (!std::isfinite(a)) return Fail("(e) NaN while towed");
      max_alt = std::max(max_alt, a);
    }
    std::fprintf(stderr, "(e) towed max altitude=%.2f m  tension=%.1f N\n", max_alt,
                 kite.state().tension_n);
    if (max_alt < 2.0f) return Fail("(e) towed kite never left the ground");
    if (kite.state().tension_n > desc.tether_max_tension)
      return Fail("(e) tow tension exceeded the cap");
  }

  // (f) 60 s of gusting wind: NaN-free and tension never exceeds the cap --------
  {
    PhysicsWorld world;
    if (!world.Initialize()) return Fail("(f) init");
    InstallGround(world);
    Kite kite(world, desc, anchor, Vec3{0, 0.6f, line * 0.95f}, 0.0f);
    if (!kite.valid()) return Fail("(f) spawn");
    f32 max_tension = 0.0f;
    for (int i = 0; i < 60 * 60; ++i) {
      const f32 t = i * kDt;
      // Violent gusting: base wind plus large multi-frequency swings and a
      // rotating cross-component.
      const f32 base = 8.0f + 7.0f * std::sin(t * 1.3f) + 4.0f * std::sin(t * 5.1f + 0.7f);
      const f32 cross = 6.0f * std::sin(t * 0.9f + 2.0f);
      world.set_wind({cross, 2.0f * std::sin(t * 3.3f), base});
      // Also jerk the anchor around to stress the one-sided spring.
      kite.set_anchor(Vec3{2.0f * std::sin(t * 2.0f), 0.0f, 2.0f * std::cos(t * 2.0f)});
      KiteInput in;
      in.steer = std::sin(t * 4.0f);
      Step(world, kite, in);
      const KiteState s = kite.state();
      if (!std::isfinite(s.position.x) || !std::isfinite(s.position.y) ||
          !std::isfinite(s.position.z) || !std::isfinite(s.tension_n) ||
          !std::isfinite(s.alpha_deg))
        return Fail("(f) NaN under gusting");
      max_tension = std::max(max_tension, s.tension_n);
    }
    std::fprintf(stderr, "(f) 60 s gust: max tension=%.1f N (cap %.0f)\n", max_tension,
                 desc.tether_max_tension);
    if (max_tension > desc.tether_max_tension + 1e-3f)
      return Fail("(f) tension exceeded the documented cap");
  }

  std::fprintf(stderr, "kite_test: all checks passed\n");
  return 0;
}
