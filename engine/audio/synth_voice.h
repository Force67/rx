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
};

}  // namespace rx::audio

#endif  // RX_AUDIO_SYNTH_VOICE_H_
