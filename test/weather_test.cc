// rx::weather acceptance: the weather-state layer as pure CPU logic -- a seeded
// scheduler that dwells and cross-fades between named states, region gating of
// which states occur where, scripted overrides, and the wind / surface /
// lightning integrals it writes into the two renderer value structs. No GPU, no
// device: everything here runs on plain scalars and the module's own PRNG.

#include "weather/weather.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace rx;
using namespace rx::weather;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "weather_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-3f) {
  if (std::fabs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "weather_test: FAIL: %s (got %.4f, expected %.4f)\n", message, actual,
               expected);
  ++failures;
}

// A calm/clear state (never rains, never storms). Distinct seeds keep the map
// endpoints comparable in the blend test.
WeatherState Clear(u32 seed) {
  WeatherState s;
  s.name = "clear";
  s.coverage = 0.2f;
  s.cloud_type = 0.7f;
  s.precipitation = 0.0f;
  s.storminess = 0.0f;
  s.map_seed = seed;
  s.wind_yaw = 0.0f;
  s.wind_speed = 6.0f;
  s.transition_seconds = 4.0f;
  s.min_dwell = 2.0f;
  s.max_dwell = 4.0f;
  return s;
}

// A heavy storm (rains hard, anvil tops -> lightning territory).
WeatherState Storm(u32 seed) {
  WeatherState s;
  s.name = "storm";
  s.coverage = 0.95f;
  s.cloud_type = 0.9f;
  s.precipitation = 1.0f;
  s.storminess = 1.0f;
  s.darkness = 0.6f;
  s.base_altitude = 1200.0f;  // cumulonimbus: low base, towering top
  s.top_altitude = 12000.0f;
  s.map_seed = seed;
  s.wind_yaw = 0.0f;
  s.wind_speed = 20.0f;
  s.transition_seconds = 4.0f;
  s.min_dwell = 2.0f;
  s.max_dwell = 4.0f;
  return s;
}

// --- Determinism: identical seed + identical Update stream -> identical run. --
void TestDeterministic() {
  auto build = [](WeatherSystem& sys) {
    sys.AddState(Clear(1));
    WeatherState a = Clear(2);
    a.coverage = 0.5f;
    sys.AddState(a);
    WeatherState b = Storm(3);
    sys.AddState(b);
    WeatherState c = Clear(4);
    c.weight = 3.0f;  // uneven weights exercise the weighted picker
    sys.AddState(c);
  };

  WeatherSystem lhs(0xC0FFEEu), rhs(0xC0FFEEu);
  build(lhs);
  build(rhs);

  bool identical = true;
  bool saw_change = false;
  u32 last = lhs.active_state();
  for (int i = 0; i < 4000; ++i) {
    lhs.Update(0.5f, Vec3{0, 0, 0}, 0.5f);
    rhs.Update(0.5f, Vec3{0, 0, 0}, 0.5f);
    if (lhs.active_state() != rhs.active_state() || lhs.target_state() != rhs.target_state() ||
        std::fabs(lhs.transition() - rhs.transition()) > 1e-6f) {
      identical = false;
    }
    if (lhs.active_state() != last) saw_change = true;
    last = lhs.active_state();
  }
  Check(identical, "same seed + same updates reproduce the exact schedule");
  Check(saw_change, "the scheduler actually transitions between states");
}

// --- Region gating: inside a region, only its allowed states ever schedule. --
void TestRegionRestriction() {
  WeatherSystem sys(42u);
  sys.AddState(Clear(1));   // 0
  sys.AddState(Clear(2));   // 1
  sys.AddState(Storm(3));   // 2
  sys.AddState(Clear(4));   // 3

  WeatherRegion r;
  r.name = "valley";
  r.min_xz = Vec2{-100.0f, -100.0f};
  r.max_xz = Vec2{100.0f, 100.0f};
  r.priority = 10;
  r.states.push_back(1u);
  r.states.push_back(2u);
  r.weights.push_back(1.0f);
  r.weights.push_back(1.0f);
  sys.AddRegion(r);

  const Vec3 inside{10.0f, 0.0f, -20.0f};
  bool all_allowed = true;
  int settled_samples = 0;
  for (int i = 0; i < 6000; ++i) {
    sys.Update(0.5f, inside, 0.5f);
    u32 a = sys.active_state();
    if (a != 1 && a != 2) all_allowed = false;
    u32 t = sys.target_state();
    if (t != 1 && t != 2) all_allowed = false;
    if (sys.transition() == 0.0f) ++settled_samples;
  }
  Check(all_allowed, "player in a region only ever gets that region's states");
  Check(settled_samples > 0, "the region run reaches settled states to sample");
}

// --- Transitions: map_blend rises monotonically, ends at 0 with map_a == b. --
void TestTransitionBlend() {
  WeatherSystem sys(7u);
  WeatherState s0 = Clear(11);
  s0.weight = 1.0f;
  WeatherState s1 = Storm(22);
  s1.weight = 0.0f;  // so the initial auto-pick lands on s0
  sys.AddState(s0);
  sys.AddState(s1);

  sys.Update(0.0f, Vec3{0, 0, 0}, 0.5f);  // settle onto s0
  Check(sys.active_state() == 0 && sys.target_state() == 0, "initial settles to the weighted state");

  sys.ForceState(1, 4.0f);  // cross-fade to the storm over 4s
  Check(sys.target_state() == 1, "ForceState retargets immediately");

  // Mid-transition, the shell altitudes and menace sit strictly between the
  // endpoints: the deck sinks/darkens WITH the cross-fade, never snapping.
  sys.Update(2.0f, Vec3{0, 0, 0}, 0.5f);
  {
    const render::CloudscapeControls& mid = sys.cloudscape();
    f32 lo_b = std::min(s0.base_altitude, s1.base_altitude);
    f32 hi_b = std::max(s0.base_altitude, s1.base_altitude);
    Check(mid.bottom > lo_b && mid.bottom < hi_b, "shell base blends through the transition");
    Check(mid.darkness > s0.darkness && mid.darkness < s1.darkness,
          "darkness blends through the transition");
  }

  f32 prev = 0.0f, peak = 0.0f;
  bool monotonic = true, settled = false;
  for (int i = 0; i < 20 && !settled; ++i) {
    sys.Update(0.5f, Vec3{0, 0, 0}, 0.5f);
    f32 mb = sys.cloudscape().map_blend;
    if (sys.active_state() == sys.target_state()) {
      settled = true;  // this frame collapsed the transition
    } else {
      if (mb < prev - 1e-4f) monotonic = false;
      prev = mb;
      if (mb > peak) peak = mb;
    }
  }
  Check(monotonic, "map_blend is monotonic through the transition");
  Check(peak > 0.5f, "map_blend climbs well toward 1 before settling");
  Check(settled, "the transition completes");

  const render::CloudscapeControls& c = sys.cloudscape();
  Near(c.map_blend, 0.0f, "settled map_blend returns to 0");
  Check(c.map_a.seed == c.map_b.seed, "settled map endpoints share a seed");
  Near(c.map_a.coverage, c.map_b.coverage, "settled map endpoints share coverage");
  Near(c.map_a.precipitation, c.map_b.precipitation, "settled map endpoints share precip");
  Check(c.map_b.seed == 22u, "settled onto the forced target's map");
}

// --- Scripted override wins and releases. --
void TestForcedOverride() {
  WeatherSystem sys(99u);
  sys.AddState(Clear(1));   // 0, weight 1
  sys.AddState(Clear(2));   // 1, weight 1
  WeatherState never = Storm(3);
  never.weight = 0.0f;      // the scheduler will never pick this on its own
  sys.AddState(never);      // 2

  sys.Update(0.1f, Vec3{0, 0, 0}, 0.5f);
  sys.ForceState(2, 0.0f);  // snap to the storm
  Check(sys.active_state() == 2 && sys.target_state() == 2, "instant force snaps active+target");

  bool stayed = true;
  for (int i = 0; i < 400; ++i) {  // far longer than any dwell
    sys.Update(0.5f, Vec3{0, 0, 0}, 0.5f);
    if (sys.active_state() != 2) stayed = false;
  }
  Check(stayed, "a forced state never dwells out");

  sys.ClearForced();
  bool resumed = false;
  for (int i = 0; i < 400; ++i) {
    sys.Update(0.5f, Vec3{0, 0, 0}, 0.5f);
    if (sys.active_state() != 2) resumed = true;  // weight-0 state is left behind
  }
  Check(resumed, "ClearForced resumes scheduling away from the forced state");
}

// --- Surface response: wetness rises in rain and dries after. --
void TestSurfaceResponse() {
  WeatherSystem sys(5u);
  sys.AddState(Storm(1));   // 0: heavy rain, not snow
  sys.AddState(Clear(2));   // 1: dry
  sys.ForceState(0, 0.0f);

  for (int i = 0; i < 40; ++i) sys.Update(0.5f, Vec3{0, 0, 0}, 0.5f);  // 20s of rain
  f32 wet = sys.weather().wetness;
  Check(wet > 0.2f, "wetness rises under rain");

  sys.ForceState(1, 0.0f);  // dry state
  for (int i = 0; i < 80; ++i) sys.Update(0.5f, Vec3{0, 0, 0}, 0.5f);  // 40s dry
  Check(sys.weather().wetness < wet, "wetness dries after the rain stops");
}

// --- Wind advection: map_offset integrates wind * dt, across a state change. --
void TestWindAdvection() {
  WeatherSystem sys(3u);
  WeatherState east = Clear(1);
  east.wind_yaw = 0.0f;     // blow toward +x
  east.wind_speed = 10.0f;
  WeatherState east2 = Clear(2);
  east2.wind_yaw = 0.0f;
  east2.wind_speed = 10.0f;
  sys.AddState(east);
  sys.AddState(east2);
  sys.ForceState(0, 0.0f);

  for (int i = 0; i < 5; ++i) sys.Update(1.0f, Vec3{0, 0, 0}, 0.5f);  // 5s
  Near(sys.cloudscape().map_offset.x, 50.0f, "map_offset integrates wind * time", 1e-2f);
  Near(sys.cloudscape().map_offset.y, 0.0f, "wind along +x leaves z offset at 0", 1e-2f);

  f32 before = sys.cloudscape().map_offset.x;
  sys.ForceState(1, 0.0f);  // change state...
  for (int i = 0; i < 5; ++i) sys.Update(1.0f, Vec3{0, 0, 0}, 0.5f);
  Check(sys.cloudscape().map_offset.x > before + 1.0f,
        "map_offset keeps advecting across a state change");
}

// --- Lightning: only stormy states strike; clear weather stays dark. --
void TestLightning() {
  // Clear weather: no strike ever, flash stays 0.
  {
    WeatherSystem sys(1234u);
    sys.AddState(Clear(1));
    sys.ForceState(0, 0.0f);
    bool any_flash = false;
    for (int i = 0; i < 400; ++i) {
      sys.Update(0.5f, Vec3{0, 0, 0}, 0.5f);
      if (sys.weather().lightning > 0.0f || sys.weather().strike_age >= 0.0f) any_flash = true;
    }
    Check(!any_flash, "clear weather never strikes");
  }
  // Storm: a strike fires within range and the flash lights up.
  {
    WeatherSystem sys(1234u);
    sys.AddState(Storm(1));
    sys.ForceState(0, 0.0f);
    bool struck = false, flashed = false, in_range = true;
    const Vec3 player{500.0f, 0.0f, -250.0f};
    for (int i = 0; i < 400; ++i) {  // ~200s, plenty for a few strikes
      sys.Update(0.5f, player, 0.5f);
      const render::WeatherSettings& w = sys.weather();
      if (w.strike_age >= 0.0f) {
        struck = true;
        f32 dx = w.strike_pos.x - player.x, dz = w.strike_pos.z - player.z;
        f32 d = std::sqrt(dx * dx + dz * dz);
        if (d < 90.0f || d > 320.0f) in_range = false;
      }
      if (w.lightning > 0.0f) flashed = true;
    }
    Check(struck, "a storm schedules a strike");
    Check(flashed, "an active strike drives the global flash");
    Check(in_range, "strikes land on the 100..300 m ring around the player");
  }
}

}  // namespace

int main() {
  TestDeterministic();
  TestRegionRestriction();
  TestTransitionBlend();
  TestForcedOverride();
  TestSurfaceResponse();
  TestWindAdvection();
  TestLightning();

  if (failures == 0) {
    std::printf("weather_test: OK\n");
    return 0;
  }
  std::fprintf(stderr, "weather_test: %d failure(s)\n", failures);
  return failures;
}
