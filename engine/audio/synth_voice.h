#ifndef RX_AUDIO_SYNTH_VOICE_H_
#define RX_AUDIO_SYNTH_VOICE_H_

#include <atomic>
#include <memory>

#include "audio/audio_clip.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::audio {

// Drivetrain telemetry that drives every procedural vehicle layer. Plain floats
// with no physics dependency: the driver fills it once per frame and the synth
// clamps on use, so out-of-range values never reach the oscillators. Ranges are
// documented per field; anything nonsensical (NaN) is treated as zero.
struct SynthParams {
  f32 rpm = 0.0f;        // engine crank speed (turbine: N1 %, 0..100)
  f32 load = 0.0f;       // 0..1 torque demand met; shapes harmonic richness
  f32 throttle = 0.0f;   // 0..1 driver pedal; gates intake noise and burble
  f32 speed_mps = 0.0f;  // ground speed, drives wind and scales skid
  f32 slip = 0.0f;       // 0..1 tyre slip ratio, gates the skid layer

  // --- additive, default-inert fields (added for the vehicle-realism pass) ---
  // Each defaults to a value that reproduces the pre-existing behaviour exactly,
  // so a caller that fills only the block above is unchanged.

  // 0..1 output muffle: as it rises the EngineSynth tightens its output low-pass
  // and ducks level, so a submerged/occluded engine reads as underwater or behind
  // a wall. 0 = dry, the preset's own voice. Smoothed here so a ventilating boat
  // prop toggling it every frame never clicks.
  f32 muffle = 0.0f;

  // Turbine (LightJet) roar level, 0..1, decoupled from spool. <0 (the default)
  // means "derive the roar from rpm/N1 as before"; >=0 lets thrust lag the spool
  // so the whine (pitched by rpm) climbs before the roar catches up. Ignored by
  // the piston path.
  f32 thrust = -1.0f;

  // One-shot gear-shift flare request, edge-triggered inside EngineSynth: <0 asks
  // for a brief upshift throttle-cut dip, >0 for a small downshift blip, 0 idles.
  // Passed through UNSMOOTHED (the synth owns the sample-accurate envelope), so
  // the rising edge is preserved; re-asserting while a flare runs does not retrig.
  f32 gear_shift = 0.0f;

  // Longitudinal slip bias for the skid band, -1 (rear axle slipping) .. +1
  // (front axle). Subtly lifts the SkidSynth band centre toward the front so a
  // front-end wash reads a touch brighter than a rear break-away. 0 = neutral.
  f32 skid_bias = 0.0f;
};

// A pull-model synthesis model that renders mono float at a fixed rate. Each
// vehicle layer (engine, skid, wind) implements it. It is stateful and phase-
// continuous, so only the render (device) thread ever touches it; parameter
// hand-off from the engine thread is the SynthVoice's job, not the model's.
class Synth {
 public:
  virtual ~Synth() = default;

  // Overwrites `frames` mono samples in `out`, advancing internal phase. `p` is
  // the already-smoothed parameter block for this sub-block, so the model treats
  // it as constant and never has to de-click updates itself. Output is bounded
  // (roughly [-1, 1]) and finite for any finite input.
  virtual void Render(f32* out, u32 frames, const SynthParams& p) = 0;
};

// The procedural voice seam: a Decoder that renders from a Synth model instead
// of a file, endlessly, directly at the mixer's output rate so the mixer never
// resamples it. Parameters are updated from the engine thread with SetParams
// while the device thread renders through Read; a seqlock hands over a coherent
// snapshot and per-sub-block one-pole smoothing keeps updates click-free.
class RX_AUDIO_EXPORT SynthVoice final : public Decoder {
 public:
  // `output_rate` must be the mixer's mix rate (Mixer::output_rate) so Read
  // needs no resampling. `synth` is the model rendered; it is owned here.
  SynthVoice(u32 output_rate, std::unique_ptr<Synth> synth);

  // --- device thread (mixer) -------------------------------------------------
  u32 channels() const override { return 1; }
  u32 sample_rate() const override { return rate_; }
  u64 frame_count() const override { return 0; }  // endless: no natural length
  // Always fills the whole request (an engine never runs out of sound), so the
  // mixer keeps the voice alive until it is explicitly stopped.
  u32 Read(f32* out, u32 frames) override;
  // The model is phase-continuous, so a restart is a no-op that always succeeds.
  bool Rewind() override { return true; }

  // --- engine thread ---------------------------------------------------------
  // Publishes the latest telemetry. Lock-free against Read; the render thread
  // ramps toward it so even a large jump never clicks.
  void SetParams(const SynthParams& p);

 private:
  SynthParams LoadTarget() const;

  u32 rate_;
  std::unique_ptr<Synth> synth_;

  // Seqlock: exactly one writer (engine thread) and one reader (device thread).
  // Odd sequence = a write is in flight; the reader retries until it reads an
  // even count unchanged across the copy.
  mutable std::atomic<u32> seq_{0};
  SynthParams shared_{};  // guarded by seq_

  // Device-thread-only smoothing state.
  SynthParams smoothed_{};
  bool primed_ = false;  // first block snaps to target (no ramp up from silence)
  // Per-field one-pole coefficients for one smoothing sub-block. rpm tracks
  // fastest so the rev limiter's bounce survives; ground speed slowest.
  f32 a_rpm_ = 1.0f;
  f32 a_load_ = 1.0f;
  f32 a_throttle_ = 1.0f;
  f32 a_speed_ = 1.0f;
  f32 a_slip_ = 1.0f;
  f32 a_muffle_ = 1.0f;
  f32 a_thrust_ = 1.0f;
  f32 a_skid_bias_ = 1.0f;
};

}  // namespace rx::audio

#endif  // RX_AUDIO_SYNTH_VOICE_H_
