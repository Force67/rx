#ifndef RX_CORE_WORLD_CLOCK_H_
#define RX_CORE_WORLD_CLOCK_H_

#include <atomic>
#include <cmath>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx {

// The in-world clock that drives the day/night cycle. Game time advances with
// real time scaled by `timescale` (game minutes per real minute); the
// application picks the timescale its world runs at (see Configure). game_days
// is the fractional days-since-start; its fractional part is the time of day.
// The clock is the single source of truth: the renderer reads it to place the
// sun and gameplay/scripting layers read and write it.
//
// Advance() runs on the main loop; a scripting guest thread may read or set the
// time concurrently, so the shared fields are atomic. The main-thread advance
// is the only writer except for the occasional script set, so the
// read-modify-write needs no stronger ordering than relaxed.
class WorldClock {
 public:
  // start_hour: time of day at start (0..24). timescale: game min / real min
  // (the application supplies its world's rate; 1 keeps game time == wall time).
  void Configure(f32 start_hour, f32 timescale) {
    game_days_.store(static_cast<f64>(Wrap24(start_hour)) / 24.0, std::memory_order_relaxed);
    set_timescale(timescale);
    real_seconds_.store(0.0, std::memory_order_relaxed);
  }

  // Advances game (and wall-clock) time by one real frame. Main thread only.
  void Advance(f64 real_seconds) {
    if (real_seconds <= 0) return;
    real_seconds_.store(real_seconds_.load(std::memory_order_relaxed) + real_seconds,
                        std::memory_order_relaxed);
    const f64 game_seconds = real_seconds * timescale_.load(std::memory_order_relaxed);
    game_days_.store(game_days_.load(std::memory_order_relaxed) + game_seconds / 86400.0,
                     std::memory_order_relaxed);
  }

  // Fractional days since start (the whole part counts elapsed days).
  f64 game_days() const { return game_days_.load(std::memory_order_relaxed); }
  void set_game_days(f64 days) { game_days_.store(days, std::memory_order_relaxed); }

  // Hour of day, 0..24.
  f32 hour() const {
    const f64 d = game_days();
    return static_cast<f32>((d - std::floor(d)) * 24.0);
  }
  // Sets the time of day, keeping the whole-day count.
  void set_hour(f32 h) {
    const f64 d = game_days();
    set_game_days(std::floor(d) + static_cast<f64>(Wrap24(h)) / 24.0);
  }

  f32 timescale() const { return timescale_.load(std::memory_order_relaxed); }
  void set_timescale(f32 t) { timescale_.store(t < 0 ? 0 : t, std::memory_order_relaxed); }

  // Real wall-clock hours elapsed since Configure.
  f32 real_hours() const {
    return static_cast<f32>(real_seconds_.load(std::memory_order_relaxed) / 3600.0);
  }

 private:
  static f32 Wrap24(f32 h) {
    h = std::fmod(h, 24.0f);
    return h < 0 ? h + 24.0f : h;
  }
  std::atomic<f64> game_days_{0.0};
  // Neutral default: game time tracks wall time 1:1 until Configure sets the
  // application's rate. The clock is game-agnostic; callers pass their timescale.
  std::atomic<f32> timescale_{1.0f};
  std::atomic<f64> real_seconds_{0.0};
};

// The directional light and sky tint for a time of day, fed straight into the
// renderer's sun settings. The sun rises due east around hour 6, is overhead at
// noon and sets due west around hour 18; outside daylight it hands over to a
// dim, cool moonlight so nights read as night rather than pure black.
struct SkyLighting {
  Vec3 sun_direction;     // render-space (Y up) travel direction of the light
  f32 sun_intensity = 0;  // directional light strength
  Vec3 sun_color;         // light tint (warm at the horizon, cool at night)
  f32 ambient = 0;        // flat fill so shadows are not crushed black
};
RX_CORE_EXPORT SkyLighting ComputeSkyLighting(f32 hour);

}  // namespace rx

#endif  // RX_CORE_WORLD_CLOCK_H_
