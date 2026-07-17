#ifndef RX_AUDIO_AUX_SYNTH_H_
#define RX_AUDIO_AUX_SYNTH_H_

#include "audio/synth_voice.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::audio {

// Tyre skid: band-passed noise that the slip ratio gates and ground speed
// scales. Silent below a slip threshold (no squeal from a rolling tyre), it
// swells in as the tyre breaks traction, with the band centre drifting up a
// little with speed so a fast slide reads brighter than a parking-lot scrub.
class RX_AUDIO_EXPORT SkidSynth final : public Synth {
 public:
  explicit SkidSynth(u32 output_rate);
  void Render(f32* out, u32 frames, const SynthParams& p) override;

 private:
  f32 rate_ = 48000.0f;
  // State-variable band-pass state and xorshift noise.
  f32 svf_low_ = 0.0f;
  f32 svf_band_ = 0.0f;
  u32 rng_ = 0x9e3779b9u;
  f32 Noise();
};

// Wind rush: speed-driven filtered noise that stays out of the way at walking
// pace and becomes a prominent roar at speed. The cutoff rises with speed, so
// the rush brightens as well as loudens the faster the vehicle goes.
class RX_AUDIO_EXPORT WindSynth final : public Synth {
 public:
  explicit WindSynth(u32 output_rate);
  void Render(f32* out, u32 frames, const SynthParams& p) override;

 private:
  f32 rate_ = 48000.0f;
  f32 lp_ = 0.0f;
  f32 hp_prev_ = 0.0f;
  u32 rng_ = 0x2545f491u;
  f32 Noise();
};

}  // namespace rx::audio

#endif  // RX_AUDIO_AUX_SYNTH_H_
