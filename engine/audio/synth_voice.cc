#include "audio/synth_voice.h"

#include <algorithm>
#include <cmath>

namespace rx::audio {
namespace {

// Parameters are ramped in sub-blocks of this many frames, independent of the
// caller's Read size, so declicking does not depend on the mixer's block length.
// ~0.7 ms at 48 kHz: fine enough that a stepped value is inaudible.
constexpr u32 kSmoothChunk = 32;

// One-pole coefficient that reaches ~63% of a step in `tau` seconds, evaluated
// once per smoothing sub-block. Clamped to (0,1]; a zero rate collapses to an
// immediate copy so a headless/degenerate rate never divides by zero.
f32 ChunkAlpha(f32 tau_seconds, u32 rate) {
  if (tau_seconds <= 0.0f || rate == 0) return 1.0f;
  const f32 dt = static_cast<f32>(kSmoothChunk) / static_cast<f32>(rate);
  return std::clamp(1.0f - std::exp(-dt / tau_seconds), 0.0f, 1.0f);
}

// Replaces a non-finite telemetry value with zero so a stray NaN upstream can
// never poison the smoother (a NaN would stick forever through the one-pole).
f32 Sane(f32 v) { return std::isfinite(v) ? v : 0.0f; }

}  // namespace

void ParamMailbox::Publish(const SynthParams& p) {
  SynthParams& clean = slot_[write_];
  clean.rpm = Sane(p.rpm);
  clean.load = std::clamp(Sane(p.load), 0.0f, 1.0f);
  clean.throttle = std::clamp(Sane(p.throttle), 0.0f, 1.0f);
  clean.speed_mps = Sane(p.speed_mps);
  clean.slip = std::clamp(Sane(p.slip), 0.0f, 1.0f);
  clean.muffle = std::clamp(Sane(p.muffle), 0.0f, 1.0f);
  // thrust keeps its <0 "derive from rpm" sentinel; only a real request is
  // clamped into 0..1 (a NaN degrades to the sentinel, i.e. old behaviour).
  clean.thrust = std::isfinite(p.thrust) ? (p.thrust < 0.0f ? -1.0f : std::clamp(p.thrust, 0.0f, 1.0f))
                                         : -1.0f;
  clean.gear_shift = std::clamp(Sane(p.gear_shift), -1.0f, 1.0f);
  clean.skid_bias = std::clamp(Sane(p.skid_bias), -1.0f, 1.0f);

  // Publish: atomically swap our filled slot in as the latest and take back the
  // slot the consumer last released. release so the slot writes above are visible
  // to the Fetch that acquires this index. write_ now names a slot the consumer
  // is not reading (the three indices stay a permutation of {0,1,2}).
  const u32 prev = shared_.exchange(write_ | kDirty, std::memory_order_release);
  write_ = prev & kIndexMask;
}

bool ParamMailbox::Fetch(SynthParams* out) {
  if (!(shared_.load(std::memory_order_acquire) & kDirty)) return false;
  // Hand our old read slot back and take the freshly published one. acquire pairs
  // with the Publish release so the slot's contents are visible before we read.
  const u32 prev = shared_.exchange(read_, std::memory_order_acquire);
  read_ = prev & kIndexMask;
  *out = slot_[read_];
  return true;
}

SynthVoice::SynthVoice(u32 output_rate, std::unique_ptr<Synth> synth)
    : rate_(output_rate ? output_rate : 48000), synth_(std::move(synth)) {
  // Time constants tuned per field: rpm fast (rev-limiter bounce), pedal/slip
  // moderate, ground speed slow (wind should swell, not flutter).
  a_rpm_ = ChunkAlpha(0.015f, rate_);
  a_load_ = ChunkAlpha(0.040f, rate_);
  a_throttle_ = ChunkAlpha(0.020f, rate_);
  a_speed_ = ChunkAlpha(0.080f, rate_);
  a_slip_ = ChunkAlpha(0.025f, rate_);
  // Muffle should swell like a ducking envelope, not flutter; thrust and the skid
  // bias track at roughly the pedal's pace.
  a_muffle_ = ChunkAlpha(0.060f, rate_);
  a_thrust_ = ChunkAlpha(0.050f, rate_);
  a_skid_bias_ = ChunkAlpha(0.050f, rate_);
}

u32 SynthVoice::Read(f32* out, u32 frames) {
  // Pick up the latest published telemetry if any; otherwise hold the last value.
  params_->Fetch(&target_);
  const SynthParams target = target_;
  if (!primed_) {
    smoothed_ = target;  // start at the real operating point, not from silence
    primed_ = true;
  }
  u32 done = 0;
  while (done < frames) {
    const u32 n = std::min<u32>(kSmoothChunk, frames - done);
    // Advance the smoothed parameters one sub-block toward the latest target.
    smoothed_.rpm += (target.rpm - smoothed_.rpm) * a_rpm_;
    smoothed_.load += (target.load - smoothed_.load) * a_load_;
    smoothed_.throttle += (target.throttle - smoothed_.throttle) * a_throttle_;
    smoothed_.speed_mps += (target.speed_mps - smoothed_.speed_mps) * a_speed_;
    smoothed_.slip += (target.slip - smoothed_.slip) * a_slip_;
    smoothed_.muffle += (target.muffle - smoothed_.muffle) * a_muffle_;
    smoothed_.skid_bias += (target.skid_bias - smoothed_.skid_bias) * a_skid_bias_;
    // Thrust: ramp only within the real 0..1 range. A <0 target is the "derive
    // from rpm" sentinel and snaps (no meaningful value to ramp toward); leaving
    // the sentinel starts the ramp from 0 so it never sweeps up through negatives.
    if (target.thrust < 0.0f) {
      smoothed_.thrust = target.thrust;
    } else {
      if (smoothed_.thrust < 0.0f) smoothed_.thrust = 0.0f;
      smoothed_.thrust += (target.thrust - smoothed_.thrust) * a_thrust_;
    }
    // Gear-shift request is an edge, not a level: hand the raw target through so
    // the synth sees the rising edge and owns the flare's sample-accurate shape.
    smoothed_.gear_shift = target.gear_shift;
    synth_->Render(out + done, n, smoothed_);
    done += n;
  }
  return frames;
}

}  // namespace rx::audio
