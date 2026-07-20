#include "weather/weather.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "render/atmosphere/lightning_envelope.h"

namespace rx::weather {
namespace {

constexpr f32 kTwoPi = 6.28318530717958648f;
constexpr f32 kPi = kTwoPi * 0.5f;

// Surface-response rates (per second, scaled by precipitation for
// accumulation). Soak is fast, drying slow -- a soaked street stays dark for a
// while after the rain stops; snow blankets steadily and melts even slower than
// roads dry.
constexpr f32 kSoakRate = 0.10f;
constexpr f32 kDryRate = 0.02f;
constexpr f32 kBlanketRate = 0.06f;
constexpr f32 kMeltRate = 0.012f;

// A state counts as stormy (spawns lightning) only when it is both heavily
// precipitating and anvil-topped; a drizzle under flat stratus stays quiet.
constexpr f32 kStormPrecip = 0.5f;
constexpr f32 kStormAnvil = 0.5f;
// Mean seconds between vortex spawns while a fully tornado-prone state is
// anvil-heavy (scaled down by partial proneness).
constexpr f32 kTornadoMeanInterval = 40.0f;

constexpr f32 kStrikeMeanInterval =
    4.0f; // seconds between strikes, exponential

// Lightning strikes land on a ring around the player: close enough to matter,
// far enough not to be on top of the camera.
constexpr f32 kStrikeMinRange = 100.0f;
constexpr f32 kStrikeMaxRange = 300.0f;

f32 Clamp01(f32 v) {
  if (!std::isfinite(v)) return 0.0f;
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
f32 Lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 LerpAngle(f32 a, f32 b, f32 t) {
  f32 delta = std::fmod(b - a + kPi, kTwoPi);
  if (delta < 0.0f)
    delta += kTwoPi;
  return a + (delta - kPi) * t;
}
f32 Smoothstep(f32 t) {
  t = Clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}

WeatherState BlendState(const WeatherState &a, const WeatherState &b, f32 t) {
  WeatherState out = a;
  out.name = "transition";
  out.coverage = Lerpf(a.coverage, b.coverage, t);
  out.cloud_type = Lerpf(a.cloud_type, b.cloud_type, t);
  out.precipitation = Lerpf(a.precipitation, b.precipitation, t);
  out.storminess = Lerpf(a.storminess, b.storminess, t);
  out.darkness = Lerpf(a.darkness, b.darkness, t);
  out.density = Lerpf(a.density, b.density, t);
  out.map_seed = t < 0.5f ? a.map_seed : b.map_seed;
  out.base_altitude = Lerpf(a.base_altitude, b.base_altitude, t);
  out.top_altitude = Lerpf(a.top_altitude, b.top_altitude, t);
  out.wind_yaw = LerpAngle(a.wind_yaw, b.wind_yaw, t);
  out.wind_speed = Lerpf(a.wind_speed, b.wind_speed, t);
  out.vertical_skew = Lerpf(a.vertical_skew, b.vertical_skew, t);
  out.turbulence = Lerpf(a.turbulence, b.turbulence, t);
  out.fog_density = Lerpf(a.fog_density, b.fog_density, t);
  out.fog_height = Lerpf(a.fog_height, b.fog_height, t);
  out.fog_churn = Lerpf(a.fog_churn, b.fog_churn, t);
  out.snow = t < 0.5f ? a.snow : b.snow;
  out.aurora = t < 0.5f ? a.aurora : b.aurora;
  out.tornado_prone = Lerpf(a.tornado_prone, b.tornado_prone, t);
  out.strike_min_range = Lerpf(a.strike_min_range, b.strike_min_range, t);
  out.strike_max_range = Lerpf(a.strike_max_range, b.strike_max_range, t);
  return out;
}

} // namespace

WeatherSystem::WeatherSystem(u32 rng_seed) : rng_(rng_seed ? rng_seed : 1u) {}

u32 WeatherSystem::AddState(const WeatherState &state) {
  WeatherState clean = state;
  auto finite_or = [](f32 value, f32 fallback) {
    return std::isfinite(value) ? value : fallback;
  };
  clean.coverage = Clamp01(clean.coverage);
  clean.cloud_type = Clamp01(clean.cloud_type);
  clean.precipitation = Clamp01(clean.precipitation);
  clean.storminess = Clamp01(clean.storminess);
  clean.darkness = Clamp01(clean.darkness);
  clean.density = std::clamp(finite_or(clean.density, 1.0f), 0.0f, 10.0f);
  clean.base_altitude =
      std::clamp(finite_or(clean.base_altitude, 1800.0f), -100000.0f, 100000.0f);
  clean.top_altitude =
      std::clamp(finite_or(clean.top_altitude, clean.base_altitude + 1.0f),
                 clean.base_altitude + 1.0f, 200000.0f);
  clean.wind_yaw = std::remainder(finite_or(clean.wind_yaw, 0.0f), kTwoPi);
  clean.wind_speed = std::clamp(finite_or(clean.wind_speed, 0.0f), 0.0f, 200.0f);
  clean.vertical_skew =
      std::clamp(finite_or(clean.vertical_skew, 0.0f), -100000.0f, 100000.0f);
  clean.turbulence = std::clamp(finite_or(clean.turbulence, 0.0f), 0.0f, 10.0f);
  clean.fog_density = Clamp01(clean.fog_density);
  clean.fog_height = std::clamp(finite_or(clean.fog_height, 90.0f), 0.1f, 100000.0f);
  clean.fog_churn = Clamp01(clean.fog_churn);
  clean.tornado_prone = Clamp01(clean.tornado_prone);
  clean.strike_min_range =
      std::clamp(finite_or(clean.strike_min_range, kStrikeMinRange), 0.0f, 1000000.0f);
  clean.strike_max_range =
      std::clamp(finite_or(clean.strike_max_range, kStrikeMaxRange),
                 clean.strike_min_range, 1000000.0f);
  clean.transition_seconds =
      std::clamp(finite_or(clean.transition_seconds, 0.0f), 0.0f, 86400.0f);
  clean.min_dwell = std::clamp(finite_or(clean.min_dwell, 0.01f), 0.01f, 86400.0f);
  clean.max_dwell =
      std::clamp(finite_or(clean.max_dwell, clean.min_dwell), clean.min_dwell, 86400.0f);
  clean.weight = std::clamp(finite_or(clean.weight, 0.0f), 0.0f, 1000000.0f);
  clean.day_weight = std::clamp(finite_or(clean.day_weight, 0.0f), 0.0f, 1000000.0f);
  clean.night_weight = std::clamp(finite_or(clean.night_weight, 0.0f), 0.0f, 1000000.0f);
  states_.push_back(clean);
  return static_cast<u32>(states_.size() - 1);
}

u32 WeatherSystem::AddRegion(const WeatherRegion &region) {
  WeatherRegion clean = region;
  if (!std::isfinite(clean.min_xz.x) || !std::isfinite(clean.min_xz.y) ||
      !std::isfinite(clean.max_xz.x) || !std::isfinite(clean.max_xz.y)) {
    clean.min_xz = {};
    clean.max_xz = {};
  }
  for (f32 &weight : clean.weights) {
    weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1000000.0f)
                                   : 0.0f;
  }
  regions_.push_back(std::move(clean));
  return static_cast<u32>(regions_.size() - 1);
}

void WeatherSystem::SetGroundHeight(GroundHeightFn fn) {
  ground_ = fn ? std::move(fn) : GroundHeightFn([](f32, f32) { return 0.0f; });
}

void WeatherSystem::ForceState(u32 state_index, f32 transition_seconds) {
  if (states_.empty())
    return;
  if (state_index >= states_.size()) {
    state_index = static_cast<u32>(states_.size() - 1);
  }
  if (!std::isfinite(transition_seconds))
    transition_seconds = 0.0f;
  forced_ = true;
  started_ = true; // a forced state suppresses the first-frame region auto-pick
  if (transition_seconds <= 0.0f ||
      (!transition_active_ && state_index == from_)) {
    from_ = to_ = state_index;
    blend_ = 0.0f;
    blend_dur_ = 0.0f;
    transition_active_ = false;
    has_transition_source_ = false;
    Compose();
  } else {
    if (transition_active_) {
      transition_source_ =
          BlendState(TransitionSource(), states_[to_], Clamp01(blend_));
      has_transition_source_ = true;
    } else {
      has_transition_source_ = false;
    }
    to_ = state_index;
    blend_ = 0.0f;
    blend_dur_ = transition_seconds;
    transition_active_ = true;
  }
}

void WeatherSystem::ClearForced() {
  forced_ = false;
  // Resume scheduling from wherever we are: if settled, arm a fresh dwell; if
  // mid-transition the dwell is armed when the transition settles.
  if (!transition_active_ && !states_.empty())
    dwell_left_ = RandomDwell(from_);
}

u32 WeatherSystem::NextU32() {
  u32 x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_ = x;
  return x;
}

f32 WeatherSystem::NextF32() {
  // Top 24 bits -> a float in [0, 1): exact, no distribution object.
  return static_cast<f32>(NextU32() >> 8) * (1.0f / 16777216.0f);
}

f32 WeatherSystem::RandomRange(f32 lo, f32 hi) {
  return lo + (hi - lo) * NextF32();
}

f32 WeatherSystem::RandomDwell(u32 state) {
  const WeatherState &s = states_[state];
  f32 lo = s.min_dwell, hi = s.max_dwell;
  if (hi < lo)
    hi = lo;
  return RandomRange(lo, hi);
}

const WeatherRegion *WeatherSystem::ActiveRegion(const Vec3 &p) const {
  const WeatherRegion *best = nullptr;
  for (const WeatherRegion &r : regions_) {
    if (r.states.empty())
      continue; // an empty region cannot own the schedule
    bool has_valid_state = false;
    for (u32 state : r.states) {
      if (state < states_.size()) {
        has_valid_state = true;
        break;
      }
    }
    if (!has_valid_state)
      continue;
    if (r.min_xz.x >= r.max_xz.x || r.min_xz.y >= r.max_xz.y)
      continue;
    if (p.x < r.min_xz.x || p.x > r.max_xz.x)
      continue;
    if (p.z < r.min_xz.y || p.z > r.max_xz.y)
      continue;
    if (!best || r.priority > best->priority)
      best = &r;
  }
  return best; // nullptr -> the implicit global fallback (all states)
}

u32 WeatherSystem::PickNext(const Vec3 &player_pos, f32 time_of_day01) {
  // Day phase in [0,1]: 0 at midnight (fully night), 1 at noon (fully day),
  // smoothly through dawn/dusk. Biases each candidate's weight toward its
  // day_weight or night_weight.
  f32 dayness = 0.5f - 0.5f * std::cos(kTwoPi * time_of_day01);

  auto effective_weight = [&](u32 idx, f32 base) -> f32 {
    if (!std::isfinite(base) || base <= 0.0f)
      return 0.0f;
    const WeatherState &s = states_[idx];
    f32 mult = Lerpf(s.night_weight, s.day_weight, dayness);
    f32 w = base * mult;
    return w > 0.0f ? w : 0.0f;
  };

  const WeatherRegion *r = ActiveRegion(player_pos);

  // Total weight, then a single walk over the same ordering with a draw in
  // [0,total). Two passes avoid any scratch allocation.
  f32 total = 0.0f;
  if (r) {
    for (u32 k = 0; k < r->states.size(); ++k) {
      if (r->states[k] >= states_.size())
        continue;
      f32 base = k < r->weights.size() ? r->weights[k] : 1.0f;
      total += effective_weight(r->states[k], base);
    }
    if (total <= 0.0f)
      for (u32 state : r->states)
        if (state == from_ && state < states_.size()) return state;
    if (total <= 0.0f)
      for (u32 state : r->states)
        if (state < states_.size()) return state;
    f32 pick = NextF32() * total, acc = 0.0f;
    for (u32 k = 0; k < r->states.size(); ++k) {
      if (r->states[k] >= states_.size())
        continue;
      f32 base = k < r->weights.size() ? r->weights[k] : 1.0f;
      acc += effective_weight(r->states[k], base);
      if (pick < acc)
        return r->states[k];
    }
    return from_;
  }

  // Global fallback: every state at its own base weight.
  for (u32 i = 0; i < states_.size(); ++i)
    total += effective_weight(i, states_[i].weight);
  if (total <= 0.0f)
    return from_;
  f32 pick = NextF32() * total, acc = 0.0f;
  for (u32 i = 0; i < states_.size(); ++i) {
    acc += effective_weight(i, states_[i].weight);
    if (pick < acc)
      return i;
  }
  return static_cast<u32>(states_.size() - 1);
}

void WeatherSystem::BeginTransition(u32 next) {
  has_transition_source_ = false;
  to_ = next;
  blend_ = 0.0f;
  blend_dur_ = states_[next].transition_seconds;
  transition_active_ = true;
  if (blend_dur_ <= 0.0f)
    Settle(); // instant cross-fade
}

void WeatherSystem::Settle() {
  from_ = to_;
  blend_ = 0.0f;
  blend_dur_ = 0.0f;
  transition_active_ = false;
  has_transition_source_ = false;
  dwell_left_ = forced_ ? 0.0f : RandomDwell(from_);
}

const WeatherState &WeatherSystem::TransitionSource() const {
  return has_transition_source_ ? transition_source_ : states_[from_];
}

render::CloudscapeMapState WeatherSystem::MapOf(const WeatherState &s) const {
  render::CloudscapeMapState m;
  m.seed = s.map_seed;
  m.coverage = s.coverage;
  m.cloud_type = s.cloud_type;
  // The map's precipitation channel means "rain falling over there", which is
  // not the same thing as WeatherSettings::precipitation ("rain falling on the
  // player"). A stormy state whose rain stays kilometres away still needs its
  // storm cells in the deck, so storminess floors the map channel.
  m.precipitation = std::max(s.precipitation, s.storminess * 0.7f);
  return m;
}

void WeatherSystem::Compose() {
  const WeatherState &a = TransitionSource();
  const WeatherState &b = states_[to_];
  bool settled = !transition_active_;
  f32 s = settled ? 0.0f : blend_; // linear blend for scalar controls
  f32 ms = settled ? 0.0f : Smoothstep(blend_); // eased cross-fade for the map

  // Cloud deck controls.
  cloudscape_.map_a = MapOf(a);
  cloudscape_.map_b = MapOf(b);
  cloudscape_.map_blend = ms; // 0 (and map_a == map_b) once settled
  cloudscape_.wind_yaw = LerpAngle(a.wind_yaw, b.wind_yaw, s);
  cloudscape_.wind_speed = Lerpf(a.wind_speed, b.wind_speed, s);
  cloudscape_.vertical_skew = Lerpf(a.vertical_skew, b.vertical_skew, s);
  cloudscape_.turbulence = Lerpf(a.turbulence, b.turbulence, s);
  cloudscape_.density = Lerpf(a.density, b.density, s);
  cloudscape_.anvil = Lerpf(a.storminess, b.storminess, s);
  cloudscape_.darkness = Lerpf(a.darkness, b.darkness, s);
  // Haze blends like everything else, and drying ground breathes mist: while
  // the surface is still wet after the rain has stopped, fog rises on top of
  // whatever the state authors, then fades as the ground dries.
  f32 authored_fog = Lerpf(a.fog_density, b.fog_density, s);
  f32 precip_now = Lerpf(a.precipitation, b.precipitation, s);
  f32 mist = wetness_ * 0.35f * Clamp01(1.0f - precip_now * 2.0f);
  cloudscape_.fog_density = Clamp01(std::max(authored_fog, mist));
  cloudscape_.fog_height = Lerpf(a.fog_height, b.fog_height, s);
  cloudscape_.fog_churn = Lerpf(a.fog_churn, b.fog_churn, s);
  // The shell tracks the class each state represents (a stratus ceiling is
  // genuinely low and thin, a storm tower genuinely enormous), so altitude
  // cross-fades with the rest of the transition.
  cloudscape_.bottom = Lerpf(a.base_altitude, b.base_altitude, s);
  cloudscape_.top = Lerpf(a.top_altitude, b.top_altitude, s);

  // Renderer weather state.
  weather_.precipitation = Lerpf(a.precipitation, b.precipitation, s);
  weather_.snow = (s < 0.5f) ? a.snow : b.snow; // discrete: no half-snow
  weather_.aurora = (s < 0.5f) ? a.aurora : b.aurora;
  weather_.wind_yaw = cloudscape_.wind_yaw;
  weather_.wind_speed = cloudscape_.wind_speed;
  weather_.gustiness = Clamp01(0.15f + cloudscape_.anvil * 0.6f);
  weather_.wetness = wetness_;
  weather_.snow_cover = snow_cover_;
}

void WeatherSystem::IntegrateSurface(f32 dt, f32 precip, bool snow) {
  // Rain wets the ground and dries slowly; the renderer treats live
  // precipitation as a floor, so this is the persistent memory on top.
  if (precip > 0.0f && !snow) {
    wetness_ += kSoakRate * precip * dt;
  } else {
    wetness_ -= kDryRate * dt;
  }
  wetness_ = Clamp01(wetness_);

  if (precip > 0.0f && snow) {
    snow_cover_ += kBlanketRate * precip * dt;
  } else {
    snow_cover_ -= kMeltRate * dt;
  }
  snow_cover_ = Clamp01(snow_cover_);
}

void WeatherSystem::IntegrateTornado(f32 dt, const Vec3 &player_pos,
                                     f32 anvil) {
  const WeatherState &a = TransitionSource();
  const WeatherState &b = states_[to_];
  f32 s = transition_active_ ? blend_ : 0.0f;
  f32 prone = Lerpf(a.tornado_prone, b.tornado_prone, s);

  if (tornado_active_) {
    tornado_age_ += dt;
    if (anvil < 0.4f)
      tornado_dur_ = std::min(tornado_dur_, tornado_age_ + 9.0f);
    // Envelope: touchdown ramp, sustained wander, rope-out. The funnel
    // travels downwind a little slower than the deck, with a lazy sideways
    // wobble so its track snakes instead of ruling a straight line.
    f32 ramp = Smoothstep(tornado_age_ / 8.0f);
    f32 out = 1.0f - Smoothstep((tornado_age_ - tornado_dur_ + 9.0f) / 9.0f);
    cloudscape_.tornado_strength = ramp * out;
    f32 speed = cloudscape_.wind_speed * 0.55f;
    f32 wob = std::sin(tornado_age_ * 0.31f) * 0.6f;
    Vec2 dir{std::cos(cloudscape_.wind_yaw), std::sin(cloudscape_.wind_yaw)};
    tornado_pos_.x += (dir.x - dir.y * wob) * speed * dt;
    tornado_pos_.y += (dir.y + dir.x * wob) * speed * dt;
    cloudscape_.tornado_pos = tornado_pos_;
    if (tornado_age_ >= tornado_dur_) {
      tornado_active_ = false;
      cloudscape_.tornado_strength = 0.0f;
      f32 u = NextF32();
      if (u < 1e-4f)
        u = 1e-4f;
      tornado_timer_ = -kTornadoMeanInterval * std::log(u);
    }
    return;
  }

  cloudscape_.tornado_strength = 0.0f;
  if (prone <= 0.01f || anvil < 0.6f)
    return;
  tornado_timer_ -= dt * prone;
  if (tornado_timer_ > 0.0f)
    return;
  // Touch down upwind of the player so the track carries the funnel past.
  Vec2 dir{std::cos(cloudscape_.wind_yaw), std::sin(cloudscape_.wind_yaw)};
  f32 upwind = RandomRange(400.0f, 1100.0f);
  f32 crosswind = RandomRange(-350.0f, 350.0f);
  tornado_pos_ = Vec2{player_pos.x - dir.x * upwind - dir.y * crosswind,
                      player_pos.z - dir.y * upwind + dir.x * crosswind};
  tornado_age_ = 0.0f;
  tornado_dur_ = RandomRange(35.0f, 90.0f);
  cloudscape_.tornado_radius = RandomRange(45.0f, 85.0f);
  cloudscape_.tornado_pos = tornado_pos_;
  tornado_active_ = true;
}

bool WeatherSystem::IntegrateLightning(f32 dt, const Vec3 &player_pos,
                                       f32 precip, f32 anvil) {
  bool spawned = false;
  // A state spawns lightning when it is anvil-topped AND either rain reaches
  // the player or the deck is authored menacing (a distant front: its rain
  // stays out there, but its cells must still discharge).
  bool stormy = anvil >= kStormAnvil &&
                (precip >= kStormPrecip || cloudscape_.darkness >= 0.5f);

  // Advance and retire the active strike (the renderer draws the bolt + flash
  // light; we only own the schedule, the strike parameters and the age clock).
  if (weather_.strike_age >= 0.0f) {
    weather_.strike_age += dt;
    if (weather_.strike_age >= render::kLightningStrikeDuration) {
      weather_.strike_age = -1.0f;
    }
  }

  // Schedule new strikes only while the blended state is stormy, and only one
  // at a time. strike_timer_ is an exponential inter-arrival draw. The ring
  // the strike lands in is authored per state (blended through transitions),
  // so a distant-front state keeps its bolts kilometres out.
  if (stormy) {
    strike_timer_ -= dt;
    if (strike_timer_ <= 0.0f && weather_.strike_age < 0.0f) {
      const WeatherState &sa = TransitionSource();
      const WeatherState &sb = states_[to_];
      f32 s = transition_active_ ? blend_ : 0.0f;
      f32 min_r = Lerpf(sa.strike_min_range, sb.strike_min_range, s);
      f32 max_r = Lerpf(sa.strike_max_range, sb.strike_max_range, s);
      if (max_r < min_r)
        std::swap(min_r, max_r);
      f32 ang = RandomRange(0.0f, kTwoPi);
      f32 range = RandomRange(min_r, max_r);
      f32 sx = player_pos.x + std::cos(ang) * range;
      f32 sz = player_pos.z + std::sin(ang) * range;
      f32 strike_ground = ground_(sx, sz);
      weather_.strike_pos =
          Vec3{sx, std::isfinite(strike_ground) ? strike_ground : 0.0f, sz};
      weather_.strike_seed = NextU32();
      weather_.strike_energy = RandomRange(0.6f, 1.0f);
      weather_.strike_age = 0.0f;
      spawned = true;
      f32 u = NextF32();
      if (u < 1e-4f)
        u = 1e-4f;
      strike_timer_ = -kStrikeMeanInterval * std::log(u);
    }
  }

  // Global flash follows the shared strike envelope so the sun/ambient/cloud
  // boost agrees with the rendered bolt; no strike -> no flash. Distance
  // attenuates it: a far strike barely lifts the light where the player
  // stands (its own corner of the sky glows via the directional term and the
  // positioned flash light instead), so a distant Unwetter never relights the
  // whole scene.
  if (weather_.strike_age >= 0.0f) {
    f32 dx = weather_.strike_pos.x - player_pos.x;
    f32 dz = weather_.strike_pos.z - player_pos.z;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    f32 near_w = 600.0f / (600.0f + dist);
    f32 falloff = 0.08f + 0.92f * near_w * near_w;
    weather_.lightning = render::LightningEnvelope(
                             weather_.strike_age, weather_.strike_seed) *
                         weather_.strike_energy * falloff;
  } else {
    weather_.lightning = 0.0f;
  }
  return spawned;
}

void WeatherSystem::Update(f32 dt, const Vec3 &player_pos, f32 time_of_day01) {
  if (states_.empty())
    return; // no states: leave the default outputs untouched
  if (!std::isfinite(dt) || dt < 0.0f)
    return;
  if (!std::isfinite(player_pos.x) || !std::isfinite(player_pos.y) ||
      !std::isfinite(player_pos.z))
    return;
  if (!std::isfinite(time_of_day01))
    time_of_day01 = 0.0f;
  dt = std::min(dt, 60.0f);

  // First frame settles onto a region-valid state (unless a script forced one).
  if (!started_) {
    started_ = true;
    if (!forced_) {
      from_ = to_ = PickNext(player_pos, time_of_day01);
      blend_ = 0.0f;
      blend_dur_ = 0.0f;
      transition_active_ = false;
      dwell_left_ = RandomDwell(from_);
    }
  }

  // Bound integration error under a long frame. Scheduler boundaries are
  // consumed exactly inside each slice; wind, surface response and stochastic
  // effects then see the state for that slice instead of applying the final
  // state retroactively across the entire dt.
  f32 remaining = dt;
  bool first_slice = true;
  bool strike_spawned = false;
  do {
    f32 slice = std::min(remaining, 0.1f);
    f32 scheduler_left = slice;
    u32 boundaries = 0;
    while (scheduler_left > 0.0f && boundaries++ < 1024u) {
      if (transition_active_) {
        if (blend_dur_ <= 0.0f) {
          Settle();
          continue;
        }
        f32 left = std::max((1.0f - blend_) * blend_dur_, 0.0f);
        if (left <= 1e-6f) {
          Settle();
          continue;
        }
        f32 step = std::min(scheduler_left, left);
        blend_ += step / blend_dur_;
        scheduler_left -= step;
        if (blend_ >= 1.0f - 1e-6f)
          Settle();
        continue;
      }
      if (forced_)
        break;
      if (dwell_left_ > 0.0f) {
        f32 step = std::min(scheduler_left, dwell_left_);
        dwell_left_ -= step;
        scheduler_left -= step;
        if (scheduler_left <= 0.0f)
          break;
      }
      u32 next = PickNext(player_pos, time_of_day01);
      if (next != from_)
        BeginTransition(next);
      else
        dwell_left_ = RandomDwell(from_);
    }

    Compose();
    if (slice > 0.0f) {
      f32 yaw = cloudscape_.wind_yaw;
      f32 speed = cloudscape_.wind_speed;
      map_offset_ += Vec2{std::cos(yaw), std::sin(yaw)} * (speed * slice);
      cloudscape_.map_offset = map_offset_;

      IntegrateSurface(slice, weather_.precipitation, weather_.snow);
      // Surface integration affects public wetness/snow and post-rain mist in
      // this same slice, not one frame later.
      Compose();
      // Keep a strike spawned during this public update observable for at least
      // one caller frame; later internal slices must not age it out unseen.
      if (!strike_spawned) {
        strike_spawned = IntegrateLightning(slice, player_pos,
                                            weather_.precipitation,
                                            cloudscape_.anvil);
      }
      IntegrateTornado(slice, player_pos, cloudscape_.anvil);
    }
    f32 local_ground = ground_(player_pos.x, player_pos.z);
    cloudscape_.fog_ground = std::isfinite(local_ground) ? local_ground : 0.0f;

    remaining -= slice;
    first_slice = false;
  } while (remaining > 0.0f || first_slice);
}

} // namespace rx::weather
