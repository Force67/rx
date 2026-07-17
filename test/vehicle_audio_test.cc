// Procedural vehicle audio acceptance test, fully headless (no device): it drives
// the synth models and the SynthVoice seam directly, and the mixer via MixInto.
// Covers that the engine renders bounded non-silent finite audio, that rpm lifts
// the dominant frequency, that load changes the sound, that the skid layer gates
// on slip, and that a mid-render parameter jump does not click.

#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/aux_synth.h"
#include "audio/engine_synth.h"
#include "audio/mixer.h"
#include "audio/synth_voice.h"
#include "audio/vehicle_audio.h"
#include "core/math.h"
#include "core/types.h"

using namespace rx;
using namespace rx::audio;

namespace {

int failures = 0;

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "vehicle_audio_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                                             \
      ++failures;                                                                       \
    }                                                                                  \
  } while (0)

constexpr u32 kRate = 48000;

bool AllFinite(const std::vector<f32>& b) {
  for (f32 v : b)
    if (!std::isfinite(v)) return false;
  return true;
}

f32 MaxAbs(const std::vector<f32>& b) {
  f32 m = 0.0f;
  for (f32 v : b) m = std::max(m, std::fabs(v));
  return m;
}

f32 Rms(const std::vector<f32>& b) {
  f64 acc = 0.0;
  for (f32 v : b) acc += static_cast<f64>(v) * v;
  return b.empty() ? 0.0f : static_cast<f32>(std::sqrt(acc / b.size()));
}

f32 MaxDelta(const std::vector<f32>& b) {
  f32 m = 0.0f;
  for (size_t i = 1; i < b.size(); ++i) m = std::max(m, std::fabs(b[i] - b[i - 1]));
  return m;
}

// One-pole low-pass in place, to isolate the firing fundamental from the noise
// bed before counting zero crossings.
void LowPass(std::vector<f32>& b, f32 cutoff) {
  const f32 a = 1.0f - std::exp(-6.28318531f * cutoff / kRate);
  f32 s = 0.0f;
  for (f32& v : b) {
    s += a * (v - s);
    v = s;
  }
}

// Zero crossings per second of a low-passed copy: a coarse dominant-frequency
// estimate (~2x the fundamental) that rises monotonically with engine speed.
f32 DominantHz(const std::vector<f32>& src) {
  std::vector<f32> b = src;
  LowPass(b, 400.0f);
  int crossings = 0;
  for (size_t i = 1; i < b.size(); ++i)
    if ((b[i - 1] <= 0.0f) != (b[i] <= 0.0f)) ++crossings;
  const f32 seconds = static_cast<f32>(b.size()) / kRate;
  return crossings / (2.0f * seconds);
}

std::vector<f32> RenderEngine(const EnginePreset& preset, const SynthParams& p, u32 frames) {
  EngineSynth synth(preset, kRate);
  std::vector<f32> buf(frames, 0.0f);
  synth.Render(buf.data(), frames, p);
  return buf;
}

// (a) The engine renders bounded, finite, non-silent audio.
void TestEngineRendersSound() {
  SynthParams p;
  p.rpm = 3500.0f;
  p.load = 0.6f;
  p.throttle = 0.7f;
  std::vector<f32> buf = RenderEngine(V8Preset(), p, 24000);
  CHECK(AllFinite(buf));
  CHECK(MaxAbs(buf) > 0.02f);   // audibly present
  CHECK(MaxAbs(buf) <= 1.001f);  // bounded (soft-limited)
  CHECK(Rms(buf) > 0.005f);

  // Every preset must be finite and bounded, including the exotic ones.
  const EnginePreset presets[] = {InlineFourCarPreset(),  V8Preset(),
                                  MotorcycleTwinPreset(),  InboardBoatPreset(),
                                  SinglePropPlanePreset(), LightJetPreset()};
  for (const EnginePreset& preset : presets) {
    SynthParams q;
    q.rpm = preset.turbine ? 85.0f : 0.5f * (preset.idle_rpm + preset.redline_rpm);
    q.load = 0.8f;
    q.throttle = 0.9f;
    q.speed_mps = 30.0f;
    std::vector<f32> b = RenderEngine(preset, q, 12000);
    CHECK(AllFinite(b));
    CHECK(MaxAbs(b) <= 1.001f);
    CHECK(MaxAbs(b) > 0.01f);
  }
}

// (b) An rpm sweep raises the dominant frequency.
void TestRpmRaisesPitch() {
  const EnginePreset preset = InlineFourCarPreset();
  SynthParams lo;
  lo.rpm = 1500.0f;
  lo.load = 0.5f;
  lo.throttle = 0.5f;
  SynthParams hi = lo;
  hi.rpm = 5000.0f;
  const f32 f_lo = DominantHz(RenderEngine(preset, lo, 24000));
  const f32 f_hi = DominantHz(RenderEngine(preset, hi, 24000));
  std::fprintf(stderr, "vehicle_audio_test: dominant Hz lo=%.1f hi=%.1f\n", f_lo, f_hi);
  CHECK(f_hi > f_lo * 1.5f);  // clearly higher, not marginal noise
}

// (c) A load change measurably changes the spectrum/amplitude.
void TestLoadChangesTimbre() {
  const EnginePreset preset = V8Preset();
  SynthParams light;
  light.rpm = 3000.0f;
  light.load = 0.1f;
  light.throttle = 0.2f;
  SynthParams heavy = light;
  heavy.load = 0.95f;
  heavy.throttle = 0.95f;
  const f32 rms_light = Rms(RenderEngine(preset, light, 24000));
  const f32 rms_heavy = Rms(RenderEngine(preset, heavy, 24000));
  std::fprintf(stderr, "vehicle_audio_test: rms light=%.4f heavy=%.4f\n", rms_light, rms_heavy);
  CHECK(rms_heavy > rms_light * 1.2f);  // richer + louder under load
}

// (d) The skid layer is silent at slip 0 and audible at slip 1.
void TestSkidGatesOnSlip() {
  SkidSynth silent(kRate);
  SynthParams rolling;
  rolling.slip = 0.0f;
  rolling.speed_mps = 20.0f;
  std::vector<f32> quiet(12000, 1.0f);  // pre-fill non-zero to prove it clears
  silent.Render(quiet.data(), 12000, rolling);
  CHECK(MaxAbs(quiet) == 0.0f);

  SkidSynth sliding(kRate);
  SynthParams slide;
  slide.slip = 1.0f;
  slide.speed_mps = 20.0f;
  std::vector<f32> loud(12000, 0.0f);
  sliding.Render(loud.data(), 12000, slide);
  CHECK(AllFinite(loud));
  CHECK(Rms(loud) > 0.01f);
  CHECK(MaxAbs(loud) <= 1.001f);

  // Wind: near-silent at a crawl, prominent at speed.
  WindSynth wind(kRate);
  SynthParams crawl;
  crawl.speed_mps = 1.0f;
  std::vector<f32> gentle(12000, 0.0f);
  wind.Render(gentle.data(), 12000, crawl);
  SynthParams fast;
  fast.speed_mps = 50.0f;
  std::vector<f32> gale(12000, 0.0f);
  wind.Render(gale.data(), 12000, fast);
  std::fprintf(stderr, "vehicle_audio_test: wind rms crawl=%.4f fast=%.4f\n", Rms(gentle),
               Rms(gale));
  CHECK(Rms(gale) > Rms(gentle) * 3.0f + 0.005f);
}

// (e) A parameter update mid-render produces no discontinuity beyond a sane
// click threshold. The boat preset's heavy exhaust low-pass keeps the noise
// floor's own sample-to-sample delta small, so a click would stand out.
void TestNoClickOnParamJump() {
  SynthVoice voice(kRate, std::unique_ptr<Synth>(new EngineSynth(InboardBoatPreset(), kRate)));
  SynthParams start;
  start.rpm = 800.0f;
  start.load = 0.1f;
  start.throttle = 0.1f;
  voice.SetParams(start);

  const u32 block = 512;
  std::vector<f32> buf(block, 0.0f);
  f32 worst = 0.0f;
  // Render a few seconds, slamming the parameters to a hard jump partway.
  for (int blk = 0; blk < 200; ++blk) {
    if (blk == 100) {
      SynthParams jump;
      jump.rpm = 3600.0f;
      jump.load = 1.0f;
      jump.throttle = 1.0f;
      voice.SetParams(jump);  // engine-thread update between device-thread reads
    }
    voice.Read(buf.data(), block);
    CHECK(AllFinite(buf));
    worst = std::max(worst, MaxDelta(buf));
  }
  std::fprintf(stderr, "vehicle_audio_test: worst sample delta=%.4f\n", worst);
  CHECK(worst < 0.15f);  // no zipper/click across the jump
}

// Integration: the whole driver through the real mixer render path, headless.
void TestMixerIntegration() {
  Mixer mixer;
  mixer.Configure(kRate);
  VehicleAudio va(mixer, MotorcycleTwinPreset());

  VehicleAudioState st;
  st.rpm = 6000.0f;
  st.load = 0.7f;
  st.throttle = 0.8f;
  st.speed_mps = 30.0f;
  st.slip = 0.6f;
  st.position = Vec3{3.0f, 0.0f, -2.0f};
  va.Update(st);

  std::vector<f32> out(1024 * 2, 0.0f);  // interleaved stereo
  f32 peak = 0.0f;
  for (int i = 0; i < 40; ++i) {
    mixer.MixInto(out.data(), 1024);
    for (f32 v : out) {
      CHECK(std::isfinite(v));
      peak = std::max(peak, std::fabs(v));
    }
  }
  CHECK(peak > 0.01f);      // the vehicle is audible in the mix
  CHECK(peak <= 1.001f);    // master soft-clip keeps it bounded

  va.Stop();
  // After a Stop the fade completes and the mix returns to silence.
  for (int i = 0; i < 40; ++i) mixer.MixInto(out.data(), 1024);
  CHECK(MaxAbs(out) < 1e-4f);
}

}  // namespace

int main() {
  TestEngineRendersSound();
  TestRpmRaisesPitch();
  TestLoadChangesTimbre();
  TestSkidGatesOnSlip();
  TestNoClickOnParamJump();
  TestMixerIntegration();
  if (failures == 0) std::printf("vehicle_audio_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
