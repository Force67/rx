#ifndef RX_WEATHER_WEATHER_H_
#define RX_WEATHER_WEATHER_H_

// rx::weather -- an art-directable weather-STATE framework (the "weather
// layer") that decides WHAT atmospheric conditions exist and where. It never
// renders anything: every frame it hands the renderer two value structs and is
// done. render::CloudscapeControls drives the volumetric cloud deck (the
// opt-in "Cloudscape" system); render::WeatherSettings drives precipitation,
// wind, surface wetness/snow response, lightning and aurora.
//
// The layer owns named WeatherStates (presets), world-space WeatherRegions
// that gate which states may occur where, a deterministic scheduler that dwells
// in a state then cross-fades to the next, a scripted override for story beats,
// and the running integrals (wind advection of the cloud map, surface soak/dry,
// lightning strike timing) that must persist across state changes.
//
// Conventions (shared with rx::scene / rx::locomotion): right-handed, +Y up,
// metres, m/s, radians. Wind yaw is the direction the wind blows TOWARD on the
// XZ plane and matches render::WeatherSettings::wind_yaw. All randomness comes
// from one seeded integer PRNG owned here (no <random> distributions), so a
// given seed and Update() call sequence reproduce bit-for-bit on any platform.

#include <base/containers/vector.h>

#include <functional>

#include "core/export.h"
#include "core/math.h"  // Vec2 (weather map XZ offset), Vec3
#include "core/types.h"
#include "render/atmosphere/cloudscape_types.h"
#include "render/core/settings.h"

// Per-module export annotation. The weather module post-dates engine/core/
// export.h's fixed macro table, so we derive RX_WEATHER_EXPORT locally from the
// shared RX_DSO_* primitives using the same RX_<MODULE>_IMPLEMENTATION selector
// rx_add_module() defines. In the default static build this expands to nothing.
#if defined(RX_WEATHER_IMPLEMENTATION)
#define RX_WEATHER_EXPORT RX_DSO_EXPORT
#else
#define RX_WEATHER_EXPORT RX_DSO_IMPORT
#endif

namespace rx::weather {

// A named preset: one fully-specified set of atmospheric conditions. Adding a
// state to the system returns its index; regions and the scheduler refer to
// states by that index. Defaults describe a calm, mostly-clear afternoon.
struct WeatherState {
  const char* name = "";  // debug label only

  // --- Cloud deck (feeds render::CloudscapeMapState + CloudscapeControls) ---
  f32 coverage = 0.4f;        // 0 clear .. 1 overcast
  f32 cloud_type = 0.6f;      // 0 stratus .. 0.5 stratocumulus .. 1 cumulus
  f32 precipitation = 0.0f;   // 0 none .. 1 heavy (also the surface-response driver)
  f32 storminess = 0.0f;      // 0..1 anvil: flattened tops, dark precipitating bases
  f32 darkness = 0.0f;        // 0..1 menace: blackens the deck (severe-storm skies)
  f32 density = 1.0f;         // global cloud density multiplier
  u32 map_seed = 1u;          // varies the spatial pattern of the weather map
  // Shell altitudes, metres ASL. Author them to the class this state
  // represents: real stratus ceilings base below ~1.2 km and stay thin,
  // stratocumulus ~0.6-1.5 km base / ~2.5 km top, cumulus bases ride the
  // condensation level (~0.5-2 km, higher in dry air), and cumulonimbus
  // towers run from a low base to a 10-16 km anvil.
  f32 base_altitude = 1800.0f;
  f32 top_altitude = 6000.0f;

  // --- Wind (shared by clouds + precipitation slant) ---
  f32 wind_yaw = 0.29146f;    // radians on XZ, direction the wind blows toward
  f32 wind_speed = 12.53f;    // m/s
  f32 vertical_skew = 700.0f;  // metres of extra downwind drift at the layer top
  f32 turbulence = 1.0f;      // curl-noise distortion of the erosion detail

  // --- Surface / sky flags ---
  bool snow = false;   // precipitation falls as snow (whitens instead of wetting)
  bool aurora = false;  // night-sky curtains up in this state

  // --- Scheduling ---
  f32 transition_seconds = 20.0f;  // cross-fade duration when this state becomes the target
  f32 min_dwell = 40.0f;           // shortest time to remain once settled (seconds)
  f32 max_dwell = 140.0f;          // longest time to remain once settled
  f32 weight = 1.0f;               // base scheduling weight (relative likelihood)
  f32 day_weight = 1.0f;           // weight multiplier at high noon
  f32 night_weight = 1.0f;         // weight multiplier at midnight (lerped by time of day)
};

// A world-space region that restricts which states may occur inside it. Regions
// are axis-aligned boxes on the XZ plane; the highest-priority region that
// contains the player wins. When no explicit region contains the player the
// system falls back to an implicit global region that allows every state at its
// own base weight, so a system with states but no regions still schedules.
struct WeatherRegion {
  const char* name = "";
  Vec2 min_xz{0, 0};  // box corner (world metres)
  Vec2 max_xz{0, 0};  // box corner; a degenerate box never contains anything
  i32 priority = 0;   // higher wins ties by containment
  base::Vector<u32> states;    // allowed state indices
  base::Vector<f32> weights;   // per-state weight, parallel to `states`
                               // (missing/short entries default to 1)
};

// The weather layer. Construct with a PRNG seed, register states and regions,
// then call Update() once per frame with the player position and the day phase.
// Read cloudscape() / weather() and hand them to the renderer.
class RX_WEATHER_EXPORT WeatherSystem {
 public:
  // Optional ground-height sampler: lightning strikes ask it for the ground y
  // at a world XZ. Defaults to the y=0 plane.
  using GroundHeightFn = std::function<f32(f32 x, f32 z)>;

  explicit WeatherSystem(u32 rng_seed = 1);

  // Registration. Returns the new index. Add every state before the regions
  // that reference it; indices are stable (append only).
  u32 AddState(const WeatherState& state);
  u32 AddRegion(const WeatherRegion& region);

  // Ground sampler for lightning strike placement (defaults to y=0).
  void SetGroundHeight(GroundHeightFn fn);

  // Scripted override for story moments: jump toward `state_index` over
  // `transition_seconds` (<= 0 snaps instantly) and stop scheduling. A forced
  // state never dwells out. ClearForced() resumes normal scheduling from the
  // current state.
  void ForceState(u32 state_index, f32 transition_seconds);
  void ClearForced();

  // Advance one frame. `time_of_day01` is 0 at midnight, 0.5 at noon, 1 at
  // midnight again -- it biases state weights (day_weight/night_weight) and is
  // the only clock the layer reads. With no states registered this is a no-op
  // that leaves the default outputs untouched (never crashes).
  void Update(f32 dt, const Vec3& player_pos, f32 time_of_day01);

  // Per-frame outputs handed to the renderer.
  const render::CloudscapeControls& cloudscape() const { return cloudscape_; }
  const render::WeatherSettings& weather() const { return weather_; }

  // Introspection. `active_state` is the state currently blended FROM (the
  // settled state once a transition completes); `target_state` is the one being
  // blended TO; `transition` is the raw 0..1 blend progress (0 when settled).
  u32 active_state() const { return from_; }
  u32 target_state() const { return to_; }
  f32 transition() const { return blend_; }

 private:
  // xorshift32: the whole layer's determinism rests on this single stream.
  u32 NextU32();
  f32 NextF32();  // [0, 1)
  f32 RandomRange(f32 lo, f32 hi);
  f32 RandomDwell(u32 state);

  // Weighted next-state pick from the region that contains `player_pos`.
  u32 PickNext(const Vec3& player_pos, f32 time_of_day01);
  const WeatherRegion* ActiveRegion(const Vec3& player_pos) const;

  void BeginTransition(u32 next);      // start a cross-fade to `next`
  void Settle();                       // collapse the transition (from_ == to_)
  void Compose();                      // write cloudscape_/weather_ from the blend
  void IntegrateSurface(f32 dt, f32 precip, bool snow);
  void IntegrateLightning(f32 dt, const Vec3& player_pos, f32 precip, f32 anvil);

  render::CloudscapeMapState MapOf(const WeatherState& s) const;

  u32 rng_;  // live PRNG state

  base::Vector<WeatherState> states_;
  base::Vector<WeatherRegion> regions_;
  GroundHeightFn ground_ = [](f32, f32) { return 0.0f; };

  // State machine. Invariant when settled: from_ == to_ and blend_ == 0.
  u32 from_ = 0;
  u32 to_ = 0;
  f32 blend_ = 0.0f;     // 0..1 progress from -> to
  f32 blend_dur_ = 0.0f;  // seconds for the active transition
  f32 dwell_left_ = 0.0f;  // seconds until the next reschedule (settled + unforced only)
  bool forced_ = false;
  bool started_ = false;   // first Update selects a region-valid initial state

  // Running integrals that must survive state changes.
  Vec2 map_offset_{0, 0};   // integrated wind advection of the weather map
  f32 wetness_ = 0.0f;
  f32 snow_cover_ = 0.0f;
  f32 strike_timer_ = 0.0f;  // seconds until the next stochastic strike (stormy only)

  render::CloudscapeControls cloudscape_;
  render::WeatherSettings weather_;
};

}  // namespace rx::weather

#endif  // RX_WEATHER_WEATHER_H_
