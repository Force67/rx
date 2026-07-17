// Headless, measured proof that the five boat-type presets
// (engine/physics/boat_profiles.h) float and handle differently, and that
// cargo load is a VISIBLE, emergent physics change (a laden hull sits deeper,
// accelerates and turns more sluggishly, planes later and loses freeboard). No
// GPU: a flat water plane is installed via set_water_height and each boat is
// driven per fixed step (Boat::Update BEFORE PhysicsWorld::Update). Every number
// is printed; the asserts check ORDERINGS and margins, not absolute values.
//
// Scenarios: (a) empty draft ordering + laden-deeper-than-empty; (b) top-speed
// ordering + laden speedboat slower; (c) planing (empty speedboat planes, laden
// does not, barge never); (d) turn-rate ordering + laden slower; (e) stability
// under an identical knock-down (dinghy capsizes, fishing rights; overloaded
// fishing rights slower); (f) overloaded barge deck near awash vs comfortable
// empty freeboard; (g) NaN-free over a minute of Gerstner chop across every
// profile and load state, including a mid-run SetCargo load transfer.

#include <cmath>
#include <cstdio>
#include <memory>

#include "core/math.h"
#include "physics/boat.h"
#include "physics/boat_profiles.h"
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
  std::fprintf(stderr, "boat_profiles_test FAIL: %s\n", what);
  return 1;
}

void InstallFlatWater(PhysicsWorld& world) {
  world.set_water_height([](const Vec3&, f32* h, Vec3* flow) {
    *h = 0.0f;
    *flow = Vec3{};
    return true;
  });
}

void Step(PhysicsWorld& world, Boat& boat, const BoatInput& in) {
  boat.Update(in, kDt);
  world.Update(kDt);
}

BoatState Run(PhysicsWorld& world, Boat& boat, const BoatInput& in, int steps) {
  for (int i = 0; i < steps; ++i) Step(world, boat, in);
  return boat.state();
}

f32 Heading(const BoatState& s) {
  const Vec3 fwd = Rotate(s.rotation, Vec3{0, 0, 1});
  return std::atan2(fwd.x, fwd.z);
}

f32 Uprightness(const BoatState& s) {
  const Vec3 up = Rotate(s.rotation, Vec3{0, 1, 0});
  return up.y;
}

const char* Name(int i) {
  static const char* kN[5] = {"dinghy", "speedboat", "jetski", "fishing", "barge"};
  return kN[i];
}
BoatDesc Profile(int i) {
  switch (i) {
    case 0: return physics::DinghyProfile();
    case 1: return physics::SpeedboatProfile();
    case 2: return physics::JetskiProfile();
    case 3: return physics::FishingBoatProfile();
    default: return physics::WorkBargeProfile();
  }
}

}  // namespace

int main() {
  PhysicsWorld probe;
  if (!probe.Initialize()) {
    std::fprintf(stderr, "boat_profiles_test: physics unavailable, skipping\n");
    return 0;
  }

  // Spawns a settled boat in its own world. Caller owns both via the out-params.
  auto make = [](PhysicsWorld& world, BoatDesc desc, f32 cargo_frac, f32 settle_s,
                 std::unique_ptr<Boat>& out) {
    world.Initialize();
    InstallFlatWater(world);
    auto boat = std::make_unique<Boat>(world, desc, Vec3{0, 0.6f, 0}, 0.0f);
    if (cargo_frac > 0.0f) boat->SetCargo(cargo_frac * desc.max_cargo_kg);
    for (int i = 0; i < static_cast<int>(settle_s * 60.0f); ++i) {
      boat->Update({}, kDt);
      world.Update(kDt);
    }
    out = std::move(boat);
  };

  // (a) Empty settle-draft ordering dinghy < speedboat < fishing < barge, and
  //     each profile's laden draft exceeds its empty draft by a real margin.
  f32 empty_draft[5] = {};
  f32 laden_draft[5] = {};
  {
    std::fprintf(stderr, "(a) settle draft (m): profile   empty   laden(full)   d\n");
    const int order[4] = {0, 1, 3, 4};  // dinghy, speedboat, fishing, barge
    for (int i = 0; i < 5; ++i) {
      const BoatDesc d = Profile(i);
      PhysicsWorld we, wl;
      std::unique_ptr<Boat> be, bl;
      make(we, d, 0.0f, 8.0f, be);
      make(wl, d, 1.0f, 8.0f, bl);
      empty_draft[i] = be->state().draft_m;
      laden_draft[i] = bl->state().draft_m;
      std::fprintf(stderr, "    %-10s %6.3f   %6.3f       +%.3f\n", Name(i), empty_draft[i],
                   laden_draft[i], laden_draft[i] - empty_draft[i]);
      const f32 margin = i >= 3 ? 0.10f : 0.03f;  // big hulls must differ by >10 cm
      if (laden_draft[i] <= empty_draft[i] + margin)
        return Fail("(a) laden draft not materially deeper than empty");
    }
    for (int k = 0; k + 1 < 4; ++k) {
      if (empty_draft[order[k]] >= empty_draft[order[k + 1]])
        return Fail("(a) empty draft ordering dinghy<speedboat<fishing<barge broken");
    }
  }

  // Sustained top speed: mean |forward_speed| over the final 5 s of a 45 s
  // full-throttle run, so a planing hull's speed ripple doesn't alias the reading.
  auto sustained_top = [&](int profile, f32 cargo) {
    PhysicsWorld w;
    std::unique_ptr<Boat> b;
    make(w, Profile(profile), cargo, 2.0f, b);
    BoatInput in;
    in.throttle = 1.0f;
    Run(w, *b, in, 60 * 40);
    f64 sum = 0;
    const int n = 60 * 5;
    for (int k = 0; k < n; ++k) {
      Step(w, *b, in);
      sum += std::fabs(b->state().forward_speed);
    }
    return static_cast<f32>(sum / n);
  };

  // (b) Full-throttle top speed: jetski/speedboat fastest, barge slowest; and a
  //     fully laden speedboat is measurably slower than empty.
  f32 top[5] = {};
  {
    std::fprintf(stderr, "(b) sustained top speed (m/s), full throttle:\n");
    for (int i = 0; i < 5; ++i) {
      top[i] = sustained_top(i, 0.0f);
      std::fprintf(stderr, "    %-10s %5.2f\n", Name(i), top[i]);
    }
    const f32 laden_speed = sustained_top(1, 1.0f);
    std::fprintf(stderr, "    speedboat(laden) %5.2f  (empty %5.2f)\n", laden_speed, top[1]);
    if (top[2] <= top[4] || top[1] <= top[4]) return Fail("(b) barge not slowest");
    if (top[4] >= top[3]) return Fail("(b) barge should be slower than the fishing boat");
    if (laden_speed >= top[1] - 0.5f) return Fail("(b) laden speedboat not measurably slower");
  }

  // (c) Planing: an empty speedboat gets up ON the plane (planing fraction high
  //     AND the hull rises so its wetted fraction drops); the same boat fully
  //     laden plows instead - it stays deep in the water (wetted near 1) so it
  //     never climbs onto the plane. The barge never planes even empty. (The
  //     planing fraction is speed-gated, so the honest "is it up on the plane"
  //     signal for a heavy hull is the wetted fraction: a planing boat rides
  //     high, a plowing one stays buried.)
  {
    auto planing_state = [&](int profile, f32 cargo, f32* wetted) {
      PhysicsWorld w;
      std::unique_ptr<Boat> b;
      make(w, Profile(profile), cargo, 2.0f, b);
      BoatInput in;
      in.throttle = 1.0f;
      Run(w, *b, in, 60 * 40);
      // Average over the final 5 s so ripple doesn't alias the reading.
      f64 pf = 0, wf = 0;
      const int n = 60 * 5;
      for (int k = 0; k < n; ++k) {
        Step(w, *b, in);
        pf += b->state().planing;
        wf += b->state().wetted;
      }
      *wetted = static_cast<f32>(wf / n);
      return static_cast<f32>(pf / n);
    };
    f32 w_empty = 0, w_laden = 0, w_barge = 0;
    const f32 sb_empty = planing_state(1, 0.0f, &w_empty);
    const f32 sb_laden = planing_state(1, 1.0f, &w_laden);
    const f32 barge_empty = planing_state(4, 0.0f, &w_barge);
    std::fprintf(stderr,
                 "(c) speedboat empty: planing=%.2f wetted=%.2f | laden: planing=%.2f wetted=%.2f | "
                 "barge empty planing=%.2f\n",
                 sb_empty, w_empty, sb_laden, w_laden, barge_empty);
    if (sb_empty < 0.5f || w_empty > 0.7f) return Fail("(c) empty speedboat did not get up on the plane");
    if (w_laden < 0.85f) return Fail("(c) laden speedboat did not plow (stayed up on the plane)");
    if (w_laden <= w_empty + 0.2f) return Fail("(c) laden hull not measurably deeper than the planing empty hull");
    if (barge_empty > 0.05f) return Fail("(c) barge planed");
  }

  // (d) Rudder-hard-over turn rate: dinghy/jetski >> barge, and a laden boat
  //     turns slower than empty (more mass + inertia).
  f32 turn[5] = {};
  {
    auto turn_rate = [&](int profile, f32 cargo) {
      PhysicsWorld w;
      std::unique_ptr<Boat> b;
      make(w, Profile(profile), cargo, 2.0f, b);
      BoatInput go;
      go.throttle = 1.0f;
      Run(w, *b, go, 60 * 8);  // get moving
      const f32 h0 = Heading(b->state());
      BoatInput t = go;
      t.steer = 1.0f;
      const BoatState s = Run(w, *b, t, 60 * 5);
      return std::fabs(Heading(s) - h0) / 5.0f;  // rad/s
    };
    std::fprintf(stderr, "(d) turn rate (rad/s), full helm:\n");
    for (int i = 0; i < 5; ++i) {
      turn[i] = turn_rate(i, 0.0f);
      std::fprintf(stderr, "    %-10s %5.3f\n", Name(i), turn[i]);
    }
    const f32 fishing_laden_turn = turn_rate(3, 1.0f);
    std::fprintf(stderr, "    fishing(laden) %5.3f  (empty %5.3f)\n", fishing_laden_turn, turn[3]);
    if (turn[0] <= turn[4] * 2.0f) return Fail("(d) dinghy not far more agile than the barge");
    if (turn[2] <= turn[4] * 2.0f) return Fail("(d) jetski not far more agile than the barge");
    if (fishing_laden_turn >= turn[3]) return Fail("(d) laden fishing boat did not turn slower");
  }

  // (e) Identical knock-down (rolled to the same beam-ends angle, then released):
  //     the dinghy laden with high crew weight goes over, the fishing boat rights
  //     itself; an overloaded fishing boat rights measurably slower than empty.
  {
    // Knock a settled boat to `heel` rad about +Z, run `secs`, return uprightness
    // at the end (1 = upright, <=0 = on its beam-ends / inverted).
    auto knockdown = [&](int profile, f32 cargo, f32 heel, f32 secs) {
      PhysicsWorld w;
      std::unique_ptr<Boat> b;
      make(w, Profile(profile), cargo, 5.0f, b);
      const Vec3 p = b->state().position;
      const f32 rot[4] = {0.0f, 0.0f, std::sin(heel * 0.5f), std::cos(heel * 0.5f)};
      w.SetBodyPosition(b->body(), p, rot);
      b->Update({}, kDt);  // refresh telemetry from the heeled pose
      return Uprightness(Run(w, *b, {}, static_cast<int>(secs * 60.0f)));
    };
    // Seconds for a knocked-down boat to recover to nearly upright (up.y > 0.9),
    // or `timeout` if it never does (capsized) - the honest "how fast does it
    // self-right" metric.
    auto time_to_right = [&](int profile, f32 cargo, f32 heel, f32 timeout) {
      PhysicsWorld w;
      std::unique_ptr<Boat> b;
      make(w, Profile(profile), cargo, 5.0f, b);
      const Vec3 p = b->state().position;
      const f32 rot[4] = {0.0f, 0.0f, std::sin(heel * 0.5f), std::cos(heel * 0.5f)};
      w.SetBodyPosition(b->body(), p, rot);
      b->Update({}, kDt);
      const int limit = static_cast<int>(timeout * 60.0f);
      for (int k = 0; k < limit; ++k) {
        Step(w, *b, {});
        if (Uprightness(b->state()) > 0.9f) return k / 60.0f;
      }
      return timeout;  // never recovered within the window
    };
    const f32 heel = 1.75f;  // ~100 deg: past beam-ends, so ballast sign decides
    const f32 dinghy_laden = knockdown(0, 1.0f, heel, 6.0f);   // final uprightness
    const f32 fishing_empty = knockdown(3, 0.0f, heel, 6.0f);
    const f32 t_empty = time_to_right(3, 0.0f, heel, 8.0f);
    const f32 t_over = time_to_right(3, 1.25f, heel, 8.0f);  // structural overload
    std::fprintf(stderr,
                 "(e) knockdown: dinghy(laden) up=%.2f (capsized) | fishing(empty) up=%.2f | "
                 "time-to-right fishing empty=%.2fs overload=%.2fs\n",
                 dinghy_laden, fishing_empty, t_empty, t_over);
    if (dinghy_laden > 0.4f) return Fail("(e) laden dinghy should have capsized (stayed over)");
    if (fishing_empty < 0.9f) return Fail("(e) fishing boat did not right itself");
    if (t_over <= t_empty * 1.2f)
      return Fail("(e) overloaded fishing boat did not right measurably slower");
  }

  // (f) Freeboard: an overloaded barge sits with its deck edge near the waterline
  //     while empty it has comfortable freeboard.
  {
    PhysicsWorld we, wo;
    std::unique_ptr<Boat> be, bo;
    make(we, Profile(4), 0.0f, 10.0f, be);
    make(wo, Profile(4), 1.25f, 10.0f, bo);  // structural overload
    const f32 fb_empty = be->state().freeboard_m;
    const f32 fb_over = bo->state().freeboard_m;
    std::fprintf(stderr, "(f) barge freeboard (m): empty=%.3f overloaded(125%%)=%.3f  (cargo %.0f kg)\n",
                 fb_empty, fb_over, bo->state().cargo_kg);
    if (fb_empty < 0.5f) return Fail("(f) empty barge freeboard not comfortable");
    if (fb_over > 0.15f) return Fail("(f) overloaded barge deck not near awash");
  }

  // (g) NaN-free over a minute of Gerstner chop for every profile and load state,
  //     with a mid-run SetCargo load transfer.
  {
    static f32 t;
    for (int i = 0; i < 5; ++i) {
      const BoatDesc d = Profile(i);
      PhysicsWorld w;
      w.Initialize();
      t = 0.0f;
      w.set_water_height([](const Vec3& p, f32* h, Vec3* flow) {
        Vec3 f{};
        *h = physics::GerstnerWaveHeight(p.x, p.z, t, &f, nullptr);
        *flow = f;
        return true;
      });
      Boat boat(w, d, Vec3{0, 0.8f, 0}, 0.0f);
      BoatInput in;
      for (int k = 0; k < 60 * 60; ++k) {
        in.throttle = std::sin(t * 0.7f);
        in.steer = std::sin(t * 0.3f);
        // Load transfer partway: ramp cargo up past the rated limit and back.
        if (k == 60 * 20) boat.SetCargo(d.max_cargo_kg);
        if (k == 60 * 30) boat.SetCargo(d.max_cargo_kg * 1.25f);
        if (k == 60 * 45) boat.SetCargo(0.0f);
        t += kDt;
        boat.Update(in, kDt);
        w.Update(kDt);
        const BoatState s = boat.state();
        if (!std::isfinite(s.position.x) || !std::isfinite(s.position.y) ||
            !std::isfinite(s.position.z) || !std::isfinite(s.rpm) ||
            !std::isfinite(s.forward_speed) || !std::isfinite(s.rotation.w) ||
            !std::isfinite(s.draft_m) || !std::isfinite(s.freeboard_m)) {
          return Fail("(g) NaN/Inf in boat state on chop");
        }
      }
      std::fprintf(stderr, "(g) %-10s survived 60 s chop + load transfer, draft=%.2f freeboard=%.2f\n",
                   Name(i), boat.state().draft_m, boat.state().freeboard_m);
    }
  }

  std::fprintf(stderr, "boat_profiles_test: all checks passed\n");
  return 0;
}
