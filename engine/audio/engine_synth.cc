#include "audio/engine_synth.h"

#include <algorithm>
#include <cmath>

namespace rx::audio {
namespace {

constexpr f32 kTwoPi = 6.28318530718f;
// Upper bound on the piston bank so the per-block gain table is a fixed stack
// array; presets clamp to it.
constexpr i32 kMaxPartials = 32;

// A phase in [0,1) turned into a sine. Wrapping the accumulator (not the angle)
// keeps precision bounded no matter how long the engine runs.
f32 SinPhase(f32 phase) { return std::sin(phase * kTwoPi); }

// Advances a normalised phase by `freq` Hz for one sample and wraps it.
f32 Advance(f32 phase, f32 freq, f32 inv_rate) {
  phase += freq * inv_rate;
  phase -= std::floor(phase);
  return phase;
}

// One-pole low-pass coefficient for a cutoff in Hz. Cutoff is clamped inside the
// stable band so a runaway parameter cannot make the filter blow up.
f32 LowpassAlpha(f32 cutoff_hz, f32 rate) {
  const f32 fc = std::clamp(cutoff_hz, 20.0f, rate * 0.49f);
  return std::clamp(1.0f - std::exp(-kTwoPi * fc / rate), 0.0f, 1.0f);
}

f32 Smoothstep(f32 edge0, f32 edge1, f32 x) {
  const f32 t = std::clamp((x - edge0) / std::max(1e-4f, edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

EnginePreset InlineFourCarPreset() {
  EnginePreset p;
  p.name = "inline4_car";
  p.cylinders = 4;
  p.unevenness = 0.06f;  // near even-fire: a smooth buzz, little half-order
  p.idle_rpm = 850.0f;
  p.redline_rpm = 7200.0f;
  p.partials = 18;
  p.brightness = 1.0f;
  p.exhaust_lowpass_hz = 7500.0f;
  p.intake_level = 0.28f;
  return p;
}

EnginePreset V8Preset() {
  EnginePreset p;
  p.name = "v8";
  p.cylinders = 8;
  p.unevenness = 0.7f;  // cross-plane: strong half-order gives the lopey rumble
  p.idle_rpm = 700.0f;
  p.redline_rpm = 6500.0f;
  p.partials = 22;
  p.brightness = 1.15f;
  p.exhaust_lowpass_hz = 5200.0f;  // fat, dark exhaust
  p.intake_level = 0.22f;
  return p;
}

EnginePreset MotorcycleTwinPreset() {
  EnginePreset p;
  p.name = "moto_twin";
  p.cylinders = 2;
  p.unevenness = 0.55f;  // V-twin uneven fire: a hard, syncopated pulse
  p.idle_rpm = 1200.0f;
  p.redline_rpm = 11000.0f;  // screams at the top
  p.partials = 20;
  p.brightness = 1.25f;
  p.exhaust_lowpass_hz = 9000.0f;
  p.intake_level = 0.30f;
  return p;
}

EnginePreset InboardBoatPreset() {
  EnginePreset p;
  p.name = "inboard_boat";
  p.cylinders = 8;
  p.unevenness = 0.3f;
  p.idle_rpm = 600.0f;
  p.redline_rpm = 4000.0f;  // marine engines turn slow
  p.partials = 16;
  p.brightness = 0.8f;
  p.exhaust_lowpass_hz = 1600.0f;  // water-muffled: heavy low-pass, gurgly
  p.intake_level = 0.12f;
  return p;
}

EnginePreset SinglePropPlanePreset() {
  EnginePreset p;
  p.name = "prop_plane";
  p.cylinders = 4;
  p.unevenness = 0.15f;
  p.idle_rpm = 700.0f;
  p.redline_rpm = 2700.0f;  // direct-drive aero engines are low-revving
  p.partials = 16;
  p.brightness = 0.9f;
  p.exhaust_lowpass_hz = 4200.0f;
  p.intake_level = 0.18f;
  p.prop_blades = 2;
  p.prop_gear_ratio = 1.0f;  // direct drive: prop rpm == engine rpm
  return p;
}

EnginePreset LightJetPreset() {
  EnginePreset p;
  p.name = "light_jet";
  p.turbine = true;
  p.idle_rpm = 20.0f;    // N1 % at ground idle
  p.redline_rpm = 100.0f;
  p.brightness = 1.0f;
  p.exhaust_lowpass_hz = 12000.0f;  // bright: the whine must come through
  p.turbine_min_hz = 800.0f;
  p.turbine_max_hz = 4600.0f;
  return p;
}

EngineSynth::EngineSynth(const EnginePreset& preset, u32 output_rate)
    : preset_(preset), rate_(static_cast<f32>(output_rate ? output_rate : 48000)) {
  inv_rate_ = 1.0f / rate_;
  preset_.cylinders = std::clamp(preset_.cylinders, 1, 16);
  preset_.partials = std::clamp(preset_.partials, 2, kMaxPartials);
}

f32 EngineSynth::Noise() {
  // xorshift32, mapped to [-1, 1). Cheap and white; the layers colour it.
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<f32>(rng_) * (2.0f / 4294967295.0f) - 1.0f;
}

void EngineSynth::Render(f32* out, u32 frames, const SynthParams& p) {
  const f32 rate = rate_;
  const f32 inv_rate = inv_rate_;
  const f32 nyquist = rate * 0.5f;

  // Clamp telemetry into a sane operating window (SynthVoice already sanitises,
  // but the model must be safe when driven directly by a test).
  const f32 load = std::clamp(p.load, 0.0f, 1.0f);
  const f32 throttle = std::clamp(p.throttle, 0.0f, 1.0f);

  // ---- Turbine path: no piston bank, a spool-tracking whine + exhaust roar ---
  if (preset_.turbine) {
    const f32 spool = std::clamp(p.rpm * 0.01f, 0.0f, 1.05f);  // rpm is N1 %
    const f32 whine_hz =
        preset_.turbine_min_hz + spool * (preset_.turbine_max_hz - preset_.turbine_min_hz);
    const f32 roar_alpha = LowpassAlpha(400.0f + spool * 2500.0f, rate);
    const f32 out_alpha = LowpassAlpha(preset_.exhaust_lowpass_hz, rate);
    const f32 whine_gain = 0.42f * spool;
    const f32 roar_gain = 0.30f * std::pow(spool, 1.4f);
    for (u32 i = 0; i < frames; ++i) {
      f32 tone = SinPhase(whine_phase_);
      // A couple of harmonics of the whine, dropped if they alias.
      if (whine_hz * 2.0f < nyquist) tone += 0.35f * SinPhase(whine_phase_ * 2.0f);
      if (whine_hz * 3.0f < nyquist) tone += 0.18f * SinPhase(whine_phase_ * 3.0f);
      roar_lp_ += roar_alpha * (Noise() - roar_lp_);
      // Roar is broadband: mix the raw noise back over the low-passed core.
      const f32 roar = 0.5f * roar_lp_ + 0.5f * (Noise() * 0.4f);
      f32 s = tone * whine_gain + roar * roar_gain;
      out_lp_ += out_alpha * (s - out_lp_);
      s = std::tanh(out_lp_);
      out[i] = std::isfinite(s) ? s : 0.0f;
      whine_phase_ = Advance(whine_phase_, whine_hz, inv_rate);
    }
    return;
  }

  // ---- Piston path ----------------------------------------------------------
  const f32 rev_freq = std::max(0.0f, p.rpm) / 60.0f;  // crank revs per second
  const f32 base_freq = rev_freq * 0.5f;               // half-order fundamental
  const i32 fire_n = preset_.cylinders;  // firing order counted in half-orders

  // Per-block partial gain table. Partial n (n = 1..partials) sits at
  // n * base_freq; even n are integer engine orders, odd n are the half-orders
  // that only exist with uneven fire.
  f32 gain[kMaxPartials + 1] = {0.0f};
  f32 gain_sum = 1e-4f;
  for (i32 n = 1; n <= preset_.partials; ++n) {
    const f32 freq = static_cast<f32>(n) * base_freq;
    if (freq < 5.0f || freq >= nyquist * 0.95f) continue;
    const f32 parity = (n % 2 == 0) ? 1.0f : preset_.unevenness;  // half-orders
    const f32 fire = (n % fire_n == 0) ? 1.0f : 0.45f;  // firing harmonics boom
    const f32 h = static_cast<f32>(n) / static_cast<f32>(fire_n);
    // Spectral tilt: high partials roll off; load and brightness lift the top,
    // so a loaded engine sounds richer and harder.
    const f32 tilt = 1.0f / (1.0f + h * h * (1.9f - 1.3f * load) / preset_.brightness);
    const f32 g = parity * fire * tilt;
    gain[n] = g;
    gain_sum += g;
  }
  const f32 norm = 1.0f / gain_sum;  // keep total amplitude bounded

  // Overrun burble: closed throttle at elevated rpm gives the lumpy pop. Depth
  // rises as throttle falls and rpm climbs above idle.
  const f32 rpm_span = std::max(1.0f, preset_.redline_rpm - preset_.idle_rpm);
  const f32 rev_frac = std::clamp((p.rpm - preset_.idle_rpm) / rpm_span, 0.0f, 1.0f);
  const f32 burble_depth = (1.0f - throttle) * Smoothstep(0.15f, 0.5f, rev_frac) * 0.8f;
  const f32 burble_hz = 7.0f + rev_frac * 9.0f;  // pulses speed up with rpm

  // Overall level: audible at idle, harder under load and throttle.
  const f32 engine_amp = 0.16f + 0.30f * load + 0.14f * throttle;

  // Noise beds. Exhaust follows load and opens up with rpm; intake follows
  // throttle (induction roar) and is brighter.
  const f32 exhaust_alpha = LowpassAlpha(500.0f + rev_frac * 3500.0f, rate);
  const f32 exhaust_level = (0.05f + 0.18f * load) * preset_.intake_level;
  const f32 intake_level = throttle * preset_.intake_level;
  const f32 out_alpha = LowpassAlpha(preset_.exhaust_lowpass_hz, rate);

  // Optional propeller blade-passing tone (single-prop aircraft).
  const f32 prop_rpm = std::max(0.0f, p.rpm) * preset_.prop_gear_ratio;
  const f32 blade_hz = static_cast<f32>(preset_.prop_blades) * prop_rpm / 60.0f;
  const bool prop_on = preset_.prop_blades > 0 && blade_hz > 5.0f && blade_hz * 2.0f < nyquist;
  const f32 prop_gain = 0.30f * (0.5f + 0.5f * throttle);

  for (u32 i = 0; i < frames; ++i) {
    // Split the bank into the low (firing-fundamental) partials the burble
    // modulates and the rest, so the pops sit on the bass, not the whole tone.
    f32 low = 0.0f;
    f32 high = 0.0f;
    for (i32 n = 1; n <= preset_.partials; ++n) {
      if (gain[n] == 0.0f) continue;
      const f32 s = SinPhase(base_phase_ * static_cast<f32>(n)) * gain[n];
      if (n <= fire_n)
        low += s;
      else
        high += s;
    }
    const f32 burble = 1.0f + burble_depth * (0.6f * SinPhase(burble_phase_) + 0.4f * Noise());
    f32 s = (low * burble + high) * norm * engine_amp;

    // Intake/exhaust noise. Exhaust is low-passed; intake is a crude high-pass
    // (difference of successive samples) for an airier hiss.
    const f32 white = Noise();
    exhaust_lp_ += exhaust_alpha * (white - exhaust_lp_);
    intake_hp_ = white - intake_prev_;
    intake_prev_ = white;
    s += exhaust_lp_ * exhaust_level + intake_hp_ * 0.15f * intake_level;

    // Propeller blade tone rides over the drone.
    if (prop_on)
      s += (SinPhase(prop_phase_) + 0.4f * SinPhase(prop_phase_ * 2.0f)) * prop_gain * 0.25f;

    // Water-muffle / exhaust low-pass, then a soft limiter keeps peaks in range.
    out_lp_ += out_alpha * (s - out_lp_);
    f32 y = std::tanh(out_lp_ * 1.3f);
    out[i] = std::isfinite(y) ? y : 0.0f;

    base_phase_ = Advance(base_phase_, base_freq, inv_rate);
    burble_phase_ = Advance(burble_phase_, burble_hz, inv_rate);
    if (prop_on) prop_phase_ = Advance(prop_phase_, blade_hz, inv_rate);
  }
}

}  // namespace rx::audio
