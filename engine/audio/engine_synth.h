#ifndef RX_AUDIO_ENGINE_SYNTH_H_
#define RX_AUDIO_ENGINE_SYNTH_H_

#include "audio/synth_voice.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::audio {

// Static description of one powerplant, all constructor data. A preset is pure
// numbers so a game can tweak or add engines without touching the DSP; the
// factory helpers below build the shipped set. Frequencies are in Hz, rpm in
// crank rpm (except a turbine, whose telemetry rpm is N1 %, see `turbine`).
struct EnginePreset {
  const char* name = "generic";

  // Piston geometry. `cylinders` sets the firing order (a four-stroke fires
  // cylinders/2 times per crank revolution). `unevenness` (0..1) is how much
  // half-order energy the fire pattern carries: 0 for a smooth even-fire four,
  // high for a cross-plane V8's lumpy rumble or an odd-fire twin.
  i32 cylinders = 4;
  f32 unevenness = 0.05f;
  f32 idle_rpm = 850.0f;
  f32 redline_rpm = 7000.0f;

  // Oscillator-bank size, counted in half-orders (partial n sits at n * rpm/120
  // Hz). More partials = richer top end but more cost; partials past Nyquist are
  // dropped automatically. `brightness` lifts the high partials overall.
  i32 partials = 18;
  f32 brightness = 1.0f;

  // Final output low-pass cutoff. Low values muffle the whole engine, which is
  // how a boat's exhaust reads as underwater/wet even before the wetness duck.
  f32 exhaust_lowpass_hz = 7000.0f;
  // Induction-noise amount at full throttle (intake roar); paired with a load-
  // driven exhaust-noise bed inside the model.
  f32 intake_level = 0.25f;

  // Single-prop aircraft: a blade-passing tone layered over the piston drone.
  // blade-pass Hz = prop_blades * (engine_rpm * prop_gear_ratio) / 60. Zero
  // blades disables the layer.
  i32 prop_blades = 0;
  f32 prop_gear_ratio = 1.0f;

  // Light jet: the piston bank is replaced by a turbine whine that tracks spool
  // plus a broadband exhaust roar. Telemetry rpm is read as N1 % (0..100), and
  // the whine sweeps turbine_min_hz..turbine_max_hz across the spool range.
  bool turbine = false;
  f32 turbine_min_hz = 700.0f;
  f32 turbine_max_hz = 4200.0f;
};

// Shipped presets. Each documents what it should sound like; the DSP notes on
// the class explain how the numbers get there. Exported: the demo/game layer in
// another DSO builds VehicleAudio from these.
RX_AUDIO_EXPORT EnginePreset InlineFourCarPreset();   // buzzy, even, wide rev range
RX_AUDIO_EXPORT EnginePreset V8Preset();              // cross-plane rumble, uneven fire emphasis
RX_AUDIO_EXPORT EnginePreset MotorcycleTwinPreset();  // high redline, hard uneven-fire pulse
RX_AUDIO_EXPORT EnginePreset InboardBoatPreset();     // low rpm, water-muffled exhaust
RX_AUDIO_EXPORT EnginePreset SinglePropPlanePreset(); // engine drone + propeller blade tone
RX_AUDIO_EXPORT EnginePreset LightJetPreset();        // turbine whine + exhaust roar

// A physically-flavoured four-stroke (or turbine) engine model. It sums a bank
// of harmonics and half-order subharmonics of the firing frequency, shapes their
// gains by load, adds overrun burble and intake/exhaust noise, and optionally a
// propeller or turbine layer. Everything is float, NaN-free and bounded; state
// (oscillator phases, filters, noise) lives here and is render-thread only.
class RX_AUDIO_EXPORT EngineSynth final : public Synth {
 public:
  // `output_rate` is the mixer mix rate; all frequencies render at it directly.
  EngineSynth(const EnginePreset& preset, u32 output_rate);

  void Render(f32* out, u32 frames, const SynthParams& p) override;

  const EnginePreset& preset() const { return preset_; }

 private:
  EnginePreset preset_;
  f32 rate_ = 48000.0f;
  f32 inv_rate_ = 1.0f / 48000.0f;

  // Phase accumulators in [0,1). The whole piston bank rides one base phase (the
  // half-order fundamental) so every partial stays phase-continuous when rpm
  // moves; the aux layers keep their own phases.
  f32 base_phase_ = 0.0f;
  f32 prop_phase_ = 0.0f;
  f32 whine_phase_ = 0.0f;
  f32 burble_phase_ = 0.0f;

  // One-pole filter states for the noise beds and the output muffle.
  f32 exhaust_lp_ = 0.0f;
  f32 intake_hp_ = 0.0f;
  f32 intake_prev_ = 0.0f;
  f32 roar_lp_ = 0.0f;
  f32 out_lp_ = 0.0f;

  // Gear-shift flare: a sample-accurate one-shot level envelope owned here (not
  // in the smoothed param block) so its timing is frame-rate-independent and a
  // headless caller driving the synth directly can trigger it too. A rising edge
  // on SynthParams.gear_shift arms it; `sign` selects an upshift cut (<0) or a
  // downshift blip (>0). `left`/`total` count down the raised-cosine bump.
  f32 shift_prev_ = 0.0f;
  i32 shift_left_ = 0;
  i32 shift_total_ = 0;
  f32 shift_sign_ = 0.0f;

  // xorshift noise state (never zero).
  u32 rng_ = 0x1234567u;

  f32 Noise();
};

}  // namespace rx::audio

#endif  // RX_AUDIO_ENGINE_SYNTH_H_
