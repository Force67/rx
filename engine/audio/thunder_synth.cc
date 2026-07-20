#include "audio/thunder_synth.h"

#include <algorithm>
#include <cmath>

namespace rx::audio {
namespace {

// A strike renders as crack + rumble with a handful of echo bumps. All state
// advances sample-by-sample inside Read, so the decoder streams at any block
// size the mixer asks for and never allocates after construction.
class ThunderDecoder final : public Decoder {
public:
  ThunderDecoder(u32 rate, u32 seed, f32 energy, f32 distance_m)
      : rate_(rate), rate_f_(static_cast<f32>(rate)),
        rng_(seed ? seed : 0x9e3779b9u) {
    energy_ = std::clamp(energy, 0.0f, 1.0f);
    f32 dist = std::max(distance_m, 0.0f);

    // Air absorption: the crack is high-frequency and dies within ~1 km; the
    // rumble survives but its brightness closes down with range.
    crack_gain_ = 1.6f * energy_ * std::exp(-dist / 900.0f);
    f32 muffle = std::min(dist / 3000.0f, 1.0f);
    f32 cutoff_hz = std::min(2600.0f - 2250.0f * muffle, rate_f_ * 0.45f);
    body_lp_k_ = std::clamp(1.0f - std::exp(-2.0f * 3.14159265f * cutoff_hz / rate_f_),
                            0.0f, 1.0f);
    f32 crack_hz = std::min(1800.0f, rate_f_ * 0.2f);
    svf_f_ = 2.0f * std::sin(3.14159265f * crack_hz / rate_f_);

    // Convert the old 48 kHz per-sample coefficients into rate-independent
    // time constants. Preserve the brown-noise variance at the reference rate.
    brown_decay_ = std::exp(std::log(0.998f) * (48000.0f / rate_f_));
    brown_gain_ = 0.03f *
                  std::sqrt((1.0f - brown_decay_ * brown_decay_) /
                            (1.0f - 0.998f * 0.998f));
    wobble_k_ = 1.0f - std::exp(-7.20054f / rate_f_);
    bump_decay_step_ = std::exp(-1.6f / rate_f_);
    global_decay_step_ = std::exp(-1.0f / ((2.2f + 2.0f * energy_) * rate_f_));
    crack_decay_step_ = std::exp(-26.0f / rate_f_);

    seconds_ = 5.0f + 4.0f * energy_;
    total_frames_ = static_cast<u64>(seconds_ * rate_f_);

    // Echo bumps: the first roll follows right behind the crack, later ones
    // are reflections off terrain/cloud at randomized delays, each softer.
    bump_t_[0] = 0.05f;
    bump_t_[1] = 0.7f + Rand01() * 0.6f;
    bump_t_[2] = 1.8f + Rand01() * 1.2f;
    bump_t_[3] = 3.2f + Rand01() * 1.8f;
    bump_w_[0] = 1.0f;
    bump_w_[1] = 0.7f + Rand01() * 0.2f;
    bump_w_[2] = 0.5f + Rand01() * 0.2f;
    bump_w_[3] = 0.35f + Rand01() * 0.15f;
  }

  u32 channels() const override { return 1; }
  u32 sample_rate() const override { return rate_; }
  u64 frame_count() const override { return total_frames_; }
  bool Rewind() override { return false; } // one-shot: the mixer retires it

  u32 Read(float *out, u32 frames) override {
    if (frame_ >= total_frames_)
      return 0;
    u32 n = static_cast<u32>(std::min<u64>(frames, total_frames_ - frame_));
    for (u32 i = 0; i < n; ++i) {
      f32 t = static_cast<f32>(frame_ + i) / rate_f_;
      f32 white = Noise();

      // Crack: band-passed burst with a razor attack. The state-variable
      // filter sits ~1.8 kHz so the transient reads as a whip, not a hiss.
      f32 crack = 0.0f;
      if (t < 0.35f) {
        svf_low_ += svf_f_ * svf_band_;
        f32 high = white - svf_low_ - 0.6f * svf_band_;
        svf_band_ += svf_f_ * high;
        crack = svf_band_ * crack_decay_ * crack_gain_;
        crack_decay_ *= crack_decay_step_;
      }

      // Rumble: brown noise (leaky integrator) through the distance lowpass.
      brown_ = brown_ * brown_decay_ + white * brown_gain_;
      body_lp_ += body_lp_k_ * (brown_ - body_lp_);
      // Envelope: overlapping echo bumps under a global decay, plus a slow
      // wobble so the roll surges instead of fading on a straight line.
      f32 env = 0.0f;
      for (int b = 0; b < 4; ++b) {
        f32 dt_b = t - bump_t_[b];
        if (dt_b > 0.0f) {
          if (bump_decay_[b] == 0.0f)
            bump_decay_[b] = std::exp(-dt_b * 1.6f);
          else
            bump_decay_[b] *= bump_decay_step_;
          env += bump_w_[b] * dt_b * bump_decay_[b] * 4.349251f;
        }
      }
      env *= global_decay_;
      global_decay_ *= global_decay_step_;
      wobble_ += wobble_k_ * (Noise() - wobble_);
      env *= 1.0f + wobble_ * 40.0f;
      f32 body = body_lp_ * env * (14.0f + 10.0f * energy_) * energy_;

      // Soft clip for weight: a close, energetic strike saturates instead of
      // spiking past full scale.
      f32 remaining =
          static_cast<f32>(total_frames_ - 1u - (frame_ + i)) / rate_f_;
      f32 tail = std::clamp(remaining / 0.4f, 0.0f, 1.0f);
      tail = tail * tail * (3.0f - 2.0f * tail);
      f32 s = (crack + body) * tail;
      out[i] = s / (1.0f + std::fabs(s)) * 0.95f; // asymptote inside full scale
    }
    frame_ += n;
    return n;
  }

private:
  f32 Rand01() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<f32>(rng_ >> 8) * (1.0f / 16777216.0f);
  }
  f32 Noise() { return Rand01() * 2.0f - 1.0f; }

  u32 rate_;
  f32 rate_f_;
  u32 rng_;
  f32 energy_ = 1.0f;
  f32 seconds_ = 6.0f;
  u64 total_frames_ = 0;
  u64 frame_ = 0;

  f32 crack_gain_ = 1.0f;
  f32 svf_f_ = 0.1f;
  f32 body_lp_k_ = 0.1f;
  f32 brown_decay_ = 0.998f;
  f32 brown_gain_ = 0.03f;
  f32 wobble_k_ = 0.00015f;
  f32 bump_decay_step_ = 1.0f;
  f32 global_decay_step_ = 1.0f;
  f32 crack_decay_step_ = 1.0f;
  f32 crack_decay_ = 1.0f;
  f32 global_decay_ = 1.0f;
  f32 bump_t_[4] = {};
  f32 bump_w_[4] = {};
  f32 bump_decay_[4] = {};

  f32 svf_low_ = 0.0f;
  f32 svf_band_ = 0.0f;
  f32 brown_ = 0.0f;
  f32 body_lp_ = 0.0f;
  f32 wobble_ = 0.0f;
};

} // namespace

std::unique_ptr<Decoder> MakeThunder(u32 output_rate, u32 seed, f32 energy,
                                     f32 distance_m) {
  if (output_rate == 0 || output_rate > 768000u || !std::isfinite(energy) ||
      !std::isfinite(distance_m))
    return nullptr;
  return std::make_unique<ThunderDecoder>(output_rate, seed, energy,
                                          distance_m);
}

} // namespace rx::audio
