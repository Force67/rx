#include "weather/weather.h"

#include <cmath>
#include <utility>

#include "render/atmosphere/lightning.h"

namespace rx::weather {
namespace {

constexpr f32 kTwoPi = 6.28318530717958648f;

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

f32 Clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
f32 Lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
f32 Smoothstep(f32 t) {
  t = Clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}

} // namespace

WeatherSystem::WeatherSystem(u32 rng_seed) : rng_(rng_seed ? rng_seed : 1u) {}

u32 WeatherSystem::AddState(const WeatherState &state) {
  states_.push_back(state);
  return static_cast<u32>(states_.size() - 1);
}

u32 WeatherSystem::AddRegion(const WeatherRegion &region) {
  regions_.push_back(region);
  return static_cast<u32>(regions_.size() - 1);
}

void WeatherSystem::SetGroundHeight(GroundHeightFn fn) {
  ground_ = fn ? std::move(fn) : GroundHeightFn([](f32, f32) { return 0.0f; });
}

void WeatherSystem::ForceState(u32 state_index, f32 transition_seconds) {
  if (!states_.empty() && state_index >= states_.size()) {
    state_index = static_cast<u32>(states_.size() - 1);
  }
  forced_ = true;
  started_ = true; // a forced state suppresses the first-frame region auto-pick
  if (transition_seconds <= 0.0f || state_index == from_) {
    from_ = to_ = state_index;
    blend_ = 0.0f;
    blend_dur_ = 0.0f;
  } else {
    to_ = state_index;
    blend_ = 0.0f;
    blend_dur_ = transition_seconds;
  }
}

void WeatherSystem::ClearForced() {
  forced_ = false;
  // Resume scheduling from wherever we are: if settled, arm a fresh dwell; if
  // mid-transition the dwell is armed when the transition settles.
  if (from_ == to_ && !states_.empty())
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
      f32 base = k < r->weights.size() ? r->weights[k] : 1.0f;
      total += effective_weight(r->states[k], base);
    }
    if (total <= 0.0f)
      return from_;
    f32 pick = NextF32() * total, acc = 0.0f;
    for (u32 k = 0; k < r->states.size(); ++k) {
      f32 base = k < r->weights.size() ? r->weights[k] : 1.0f;
      acc += effective_weight(r->states[k], base);
      if (pick < acc)
        return r->states[k];
    }
    return r->states[r->states.size() - 1];
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
  to_ = next;
  blend_ = 0.0f;
  blend_dur_ = states_[next].transition_seconds;
  if (blend_dur_ <= 0.0f)
    Settle(); // instant cross-fade
}

void WeatherSystem::Settle() {
  from_ = to_;
  blend_ = 0.0f;
  blend_dur_ = 0.0f;
  dwell_left_ = RandomDwell(from_);
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
  const WeatherState &a = states_[from_];
  const WeatherState &b = states_[to_];
  bool settled = from_ == to_;
  f32 s = settled ? 0.0f : blend_; // linear blend for scalar controls
  f32 ms = settled ? 0.0f : Smoothstep(blend_); // eased cross-fade for the map

  // Cloud deck controls.
  cloudscape_.map_a = MapOf(a);
  cloudscape_.map_b = MapOf(b);
  cloudscape_.map_blend = ms; // 0 (and map_a == map_b) once settled
  cloudscape_.wind_yaw = Lerpf(a.wind_yaw, b.wind_yaw, s);
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
  const WeatherState &a = states_[from_];
  const WeatherState &b = states_[to_];
  f32 s = from_ == to_ ? 0.0f : blend_;
  f32 prone = Lerpf(a.tornado_prone, b.tornado_prone, s);

  if (tornado_active_) {
    tornado_age_ += dt;
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
    if (tornado_age_ >= tornado_dur_ || anvil < 0.4f) {
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
  f32 ang = RandomRange(0.0f, kTwoPi);
  f32 range = RandomRange(400.0f, 1100.0f);
  Vec2 dir{std::cos(cloudscape_.wind_yaw), std::sin(cloudscape_.wind_yaw)};
  tornado_pos_ = Vec2{player_pos.x + std::cos(ang) * range - dir.x * 300.0f,
                      player_pos.z + std::sin(ang) * range - dir.y * 300.0f};
  tornado_age_ = 0.0f;
  tornado_dur_ = RandomRange(35.0f, 90.0f);
  cloudscape_.tornado_radius = RandomRange(45.0f, 85.0f);
  cloudscape_.tornado_pos = tornado_pos_;
  tornado_active_ = true;
}

void WeatherSystem::IntegrateLightning(f32 dt, const Vec3 &player_pos,
                                       f32 precip, f32 anvil) {
  // A state spawns lightning when it is anvil-topped AND either rain reaches
  // the player or the deck is authored menacing (a distant front: its rain
  // stays out there, but its cells must still discharge).
  bool stormy = anvil >= kStormAnvil &&
                (precip >= kStormPrecip || cloudscape_.darkness >= 0.5f);

  // Advance and retire the active strike (the renderer draws the bolt + flash
  // light; we only own the schedule, the strike parameters and the age clock).
  if (weather_.strike_age >= 0.0f) {
    weather_.strike_age += dt;
    if (weather_.strike_age >= render::LightningSystem::kStrikeDuration) {
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
      const WeatherState &sa = states_[from_];
      const WeatherState &sb = states_[to_];
      f32 s = from_ == to_ ? 0.0f : blend_;
      f32 min_r = Lerpf(sa.strike_min_range, sb.strike_min_range, s);
      f32 max_r = Lerpf(sa.strike_max_range, sb.strike_max_range, s);
      f32 ang = RandomRange(0.0f, kTwoPi);
      f32 range = RandomRange(min_r, max_r);
      f32 sx = player_pos.x + std::cos(ang) * range;
      f32 sz = player_pos.z + std::sin(ang) * range;
      weather_.strike_pos = Vec3{sx, ground_(sx, sz), sz};
      weather_.strike_seed = NextU32();
      weather_.strike_energy = RandomRange(0.6f, 1.0f);
      weather_.strike_age = 0.0f;
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
    weather_.lightning = render::LightningSystem::Envelope(
                             weather_.strike_age, weather_.strike_seed) *
                         weather_.strike_energy * falloff;
  } else {
    weather_.lightning = 0.0f;
  }
}

void WeatherSystem::Update(f32 dt, const Vec3 &player_pos, f32 time_of_day01) {
  if (states_.empty())
    return; // no states: leave the default outputs untouched

  // First frame settles onto a region-valid state (unless a script forced one).
  if (!started_) {
    started_ = true;
    if (!forced_) {
      from_ = to_ = PickNext(player_pos, time_of_day01);
      blend_ = 0.0f;
      blend_dur_ = 0.0f;
      dwell_left_ = RandomDwell(from_);
    }
  }

  if (from_ != to_) {
    // Cross-fading toward the target.
    if (blend_dur_ <= 0.0f) {
      Settle();
    } else {
      blend_ += dt / blend_dur_;
      if (blend_ >= 1.0f)
        Settle();
    }
  } else if (!forced_) {
    // Settled and scheduling: dwell down, then pick the next state.
    dwell_left_ -= dt;
    if (dwell_left_ <= 0.0f) {
      u32 next = PickNext(player_pos, time_of_day01);
      if (next != from_) {
        BeginTransition(next);
      } else {
        dwell_left_ = RandomDwell(from_); // re-roll the dwell, same weather
      }
    }
  }

  Compose();

  // Wind advection integral: the map keeps scrolling with the wind across state
  // changes because map_offset_ is never reset, only accumulated.
  f32 yaw = cloudscape_.wind_yaw;
  f32 speed = cloudscape_.wind_speed;
  map_offset_ += Vec2{std::cos(yaw), std::sin(yaw)} * (speed * dt);
  cloudscape_.map_offset = map_offset_;

  IntegrateSurface(dt, weather_.precipitation, weather_.snow);
  IntegrateLightning(dt, player_pos, weather_.precipitation, cloudscape_.anvil);
  IntegrateTornado(dt, player_pos, cloudscape_.anvil);
}

} // namespace rx::weather
