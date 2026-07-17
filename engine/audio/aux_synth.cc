#include "audio/aux_synth.h"

#include <algorithm>
#include <cmath>

namespace rx::audio {
namespace {

constexpr f32 kPi = 3.14159265359f;

// Speed at which each layer reaches full presence (m/s). Skid saturates quickly
// (a slide is a slide); wind keeps building toward highway speed.
constexpr f32 kSkidFullSpeed = 12.0f;
constexpr f32 kWindFullSpeed = 55.0f;

// Below this slip ratio a tyre is rolling, not sliding: the skid layer is fully
// gated so a cruising vehicle is silent here.
constexpr f32 kSlipFloor = 0.08f;

f32 Smoothstep(f32 edge0, f32 edge1, f32 x) {
  const f32 t = std::clamp((x - edge0) / std::max(1e-4f, edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

SkidSynth::SkidSynth(u32 output_rate)
    : rate_(static_cast<f32>(output_rate ? output_rate : 48000)) {}

f32 SkidSynth::Noise() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<f32>(rng_) * (2.0f / 4294967295.0f) - 1.0f;
}

void SkidSynth::Render(f32* out, u32 frames, const SynthParams& p) {
  const f32 slip = std::clamp(p.slip, 0.0f, 1.0f);
  const f32 speed = std::max(0.0f, p.speed_mps);

  // Gate: slip opens the layer above the floor, speed scales how loud a slide of
  // a given slip is. No slip or no motion -> exactly silent.
  const f32 slip_gate = Smoothstep(kSlipFloor, 0.35f, slip);
  const f32 speed_scale = Smoothstep(0.5f, kSkidFullSpeed, speed);
  const f32 level = slip_gate * speed_scale * 0.6f;
  if (level <= 0.0f) {
    std::fill(out, out + frames, 0.0f);
    return;
  }

  // Band-pass centre climbs a little with speed; a fast slide squeals higher.
  const f32 centre_hz = std::clamp(900.0f + speed * 25.0f, 300.0f, rate_ * 0.4f);
  const f32 f = std::clamp(2.0f * std::sin(kPi * centre_hz / rate_), 0.0f, 1.0f);
  const f32 q = 0.28f;  // moderate resonance: a band, not a whistle

  for (u32 i = 0; i < frames; ++i) {
    const f32 in = Noise();
    // Chamberlin state-variable filter; the band-pass tap is what we keep.
    svf_low_ += f * svf_band_;
    const f32 high = in - svf_low_ - q * svf_band_;
    svf_band_ += f * high;
    f32 s = svf_band_ * level;
    s = std::tanh(s);
    out[i] = std::isfinite(s) ? s : 0.0f;
  }
}

WindSynth::WindSynth(u32 output_rate)
    : rate_(static_cast<f32>(output_rate ? output_rate : 48000)) {}

f32 WindSynth::Noise() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<f32>(rng_) * (2.0f / 4294967295.0f) - 1.0f;
}

void WindSynth::Render(f32* out, u32 frames, const SynthParams& p) {
  const f32 speed = std::max(0.0f, p.speed_mps);
  // Presence grows faster than linearly so wind is a non-issue around town and
  // dominant on the motorway.
  const f32 t = Smoothstep(3.0f, kWindFullSpeed, speed);
  const f32 level = t * t * 0.5f;
  if (level <= 0.0f) {
    std::fill(out, out + frames, 0.0f);
    return;
  }

  // Cutoff opens with speed: faster air reads brighter, not just louder.
  const f32 cutoff = std::clamp(300.0f + speed * 55.0f, 200.0f, rate_ * 0.45f);
  const f32 alpha = std::clamp(1.0f - std::exp(-2.0f * kPi * cutoff / rate_), 0.0f, 1.0f);

  for (u32 i = 0; i < frames; ++i) {
    const f32 white = Noise();
    lp_ += alpha * (white - lp_);
    // Remove the DC-ish rumble with a light high-pass so it reads as rushing air
    // rather than a low roar.
    const f32 band = lp_ - hp_prev_ * 0.02f;
    hp_prev_ = lp_;
    f32 s = std::tanh(band * level);
    out[i] = std::isfinite(s) ? s : 0.0f;
  }
}

}  // namespace rx::audio
