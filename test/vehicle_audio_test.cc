// Procedural vehicle audio acceptance test, fully headless (no device): it drives
// the synth models and the SynthVoice seam directly, and the mixer via MixInto.
// Covers that the engine renders bounded non-silent finite audio, that rpm lifts
// the dominant frequency, that load changes the sound, that the skid layer gates
// on slip, and that a mid-render parameter jump does not click.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
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

// RMS of the first difference: a cheap high-frequency-energy proxy. Muffling the
// output (a low-pass) drops this far more than it drops the total RMS, so their
// ratio is a coarse spectral-centroid stand-in with no FFT.
f32 HighFreqRms(const std::vector<f32>& b) {
  f64 acc = 0.0;
  for (size_t i = 1; i < b.size(); ++i) {
    const f32 d = b[i] - b[i - 1];
    acc += static_cast<f64>(d) * d;
  }
  return b.size() < 2 ? 0.0f : static_cast<f32>(std::sqrt(acc / (b.size() - 1)));
}

f32 HfFraction(const std::vector<f32>& b) {
  const f32 r = Rms(b);
  return r > 1e-9f ? HighFreqRms(b) / r : 0.0f;
}

// Zero crossings per second of the raw (un-low-passed) signal: rises with the
// brightest tone present, so it tracks the jet whine's pitch (the roar is low).
f32 RawZeroCrossHz(const std::vector<f32>& b) {
  int crossings = 0;
  for (size_t i = 1; i < b.size(); ++i)
    if ((b[i - 1] <= 0.0f) != (b[i] <= 0.0f)) ++crossings;
  const f32 seconds = static_cast<f32>(b.size()) / kRate;
  return crossings / (2.0f * seconds);
}

// RMS of one channel of an interleaved stereo buffer (0 = left, 1 = right).
f32 ChannelRms(const std::vector<f32>& interleaved, int ch) {
  f64 acc = 0.0;
  size_t n = 0;
  for (size_t i = ch; i < interleaved.size(); i += 2) {
    acc += static_cast<f64>(interleaved[i]) * interleaved[i];
    ++n;
  }
  return n == 0 ? 0.0f : static_cast<f32>(std::sqrt(acc / n));
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

// (1) Muffle darkens the spectrum and ducks the level, and toggling it live does
// not click.
void TestMuffleDarkensAndDucks() {
  const EnginePreset preset = InlineFourCarPreset();  // bright, room to muffle
  SynthParams dry;
  dry.rpm = 3200.0f;
  dry.load = 0.6f;
  dry.throttle = 0.6f;
  SynthParams wet = dry;
  wet.muffle = 1.0f;
  const std::vector<f32> a = RenderEngine(preset, dry, 24000);
  const std::vector<f32> b = RenderEngine(preset, wet, 24000);
  const f32 rms_dry = Rms(a), rms_wet = Rms(b);
  const f32 hf_dry = HfFraction(a), hf_wet = HfFraction(b);
  std::fprintf(stderr, "vehicle_audio_test: muffle rms dry=%.4f wet=%.4f | hf-frac dry=%.4f wet=%.4f\n",
               rms_dry, rms_wet, hf_dry, hf_wet);
  CHECK(AllFinite(b));
  CHECK(MaxAbs(b) <= 1.001f);
  CHECK(rms_wet < rms_dry * 0.9f);    // level ducked
  CHECK(hf_wet < hf_dry * 0.8f);      // spectrum darkened (relative HF gone)

  // A live submerge toggle through the SynthVoice must not click.
  SynthVoice voice(kRate, std::unique_ptr<Synth>(new EngineSynth(InboardBoatPreset(), kRate)));
  SynthParams p;
  p.rpm = 1500.0f;
  p.load = 0.4f;
  p.throttle = 0.4f;
  voice.SetParams(p);
  std::vector<f32> buf(512, 0.0f);
  f32 worst = 0.0f;
  for (int blk = 0; blk < 200; ++blk) {
    if (blk == 60) {
      p.muffle = 1.0f;  // submerge
      voice.SetParams(p);
    }
    if (blk == 130) {
      p.muffle = 0.0f;  // surface again
      voice.SetParams(p);
    }
    voice.Read(buf.data(), 512);
    CHECK(AllFinite(buf));
    worst = std::max(worst, MaxDelta(buf));
  }
  std::fprintf(stderr, "vehicle_audio_test: muffle toggle worst delta=%.4f\n", worst);
  CHECK(worst < 0.15f);
}

// (2) A signed throttle uses its magnitude: astern (-x) sounds identical to ahead
// (+x), so the boat's -1..1 range needs no change at the call site.
void TestSignedThrottleEquivalence() {
  auto run = [](f32 throttle) {
    Mixer mixer;
    mixer.Configure(kRate);
    VehicleAudio va(mixer, InboardBoatPreset());
    VehicleAudioState st;
    st.rpm = 2200.0f;
    st.load = 0.5f;
    st.throttle = throttle;
    st.speed_mps = 8.0f;
    va.Update(st);
    std::vector<f32> out(1024 * 2, 0.0f);
    std::vector<f32> acc;
    for (int i = 0; i < 20; ++i) {
      mixer.MixInto(out.data(), 1024);
      acc.insert(acc.end(), out.begin(), out.end());
    }
    return acc;
  };
  const std::vector<f32> ahead = run(0.6f);
  const std::vector<f32> astern = run(-0.6f);
  CHECK(ahead.size() == astern.size());
  f32 diff = 0.0f;
  for (size_t i = 0; i < ahead.size(); ++i) diff = std::max(diff, std::fabs(ahead[i] - astern[i]));
  std::fprintf(stderr, "vehicle_audio_test: signed-throttle max diff=%.6g\n", diff);
  CHECK(Rms(ahead) > 0.005f);  // audible (so the equivalence is not trivial silence)
  CHECK(diff < 1e-6f);         // |−0.6| == |+0.6|: bit-for-bit identical
}

// (3) Per-wheel slip pans the skid toward the slipping side and falls back exactly
// to the aggregate when no per-wheel data is present.
void TestPerWheelSlipPansSkid() {
  // Listener faces +Z (sitting in the vehicle): the vehicle's right side is −X,
  // which is then the listener's right ear.
  auto render_side = [](const f32 slips[4], u32 count, f32 agg_slip) {
    Mixer mixer;
    mixer.Configure(kRate);
    Listener lis;
    lis.position = Vec3{0.0f, 0.0f, 0.0f};
    lis.forward = Vec3{0.0f, 0.0f, 1.0f};
    lis.up = Vec3{0.0f, 1.0f, 0.0f};
    mixer.SetListener(lis);
    VehicleAudio va(mixer, InlineFourCarPreset());
    VehicleAudioState st;
    st.rpm = 0.0f;  // keep the engine near-silent so the skid dominates the pan
    st.load = 0.0f;
    st.throttle = 0.0f;
    st.speed_mps = 20.0f;
    st.slip = agg_slip;
    st.wheel_count = count;
    for (int i = 0; i < 4; ++i) st.wheel_slip[i] = slips[i];
    st.position = Vec3{0.0f, 0.0f, 4.0f};
    va.Update(st);
    std::vector<f32> out(1024 * 2, 0.0f);
    std::vector<f32> acc;
    for (int i = 0; i < 40; ++i) {  // let the pan/gain envelopes settle
      mixer.MixInto(out.data(), 1024);
      if (i >= 10) acc.insert(acc.end(), out.begin(), out.end());
    }
    return acc;
  };
  const f32 right_heavy[4] = {0.1f, 1.0f, 0.1f, 1.0f};  // FR, RR washing
  const f32 left_heavy[4] = {1.0f, 0.1f, 1.0f, 0.1f};   // FL, RL washing
  const f32 balanced[4] = {0.7f, 0.7f, 0.7f, 0.7f};

  const std::vector<f32> r = render_side(right_heavy, 4, 0.0f);
  const std::vector<f32> l = render_side(left_heavy, 4, 0.0f);
  const f32 rL = ChannelRms(r, 0), rR = ChannelRms(r, 1);
  const f32 lL = ChannelRms(l, 0), lR = ChannelRms(l, 1);
  std::fprintf(stderr, "vehicle_audio_test: skid pan right L=%.4f R=%.4f | left L=%.4f R=%.4f\n",
               rL, rR, lL, lR);
  CHECK(rR > rL * 1.05f);  // right-wheel slip -> right ear louder
  CHECK(lL > lR * 1.05f);  // left-wheel slip  -> left ear louder

  // Fallback: aggregate slip (count 0) and four equal wheels of the same value
  // produce identical audio (same intensity, no pan bias).
  const std::vector<f32> agg = render_side(balanced, 0, 0.7f);
  const std::vector<f32> quad = render_side(balanced, 4, 0.0f);
  f32 diff = 0.0f;
  for (size_t i = 0; i < std::min(agg.size(), quad.size()); ++i)
    diff = std::max(diff, std::fabs(agg[i] - quad[i]));
  const f32 aL = ChannelRms(agg, 0), aR = ChannelRms(agg, 1);
  std::fprintf(stderr, "vehicle_audio_test: skid fallback diff=%.6g balanced L=%.4f R=%.4f\n", diff,
               aL, aR);
  CHECK(Rms(agg) > 0.005f);                       // the skid is actually audible
  CHECK(diff < 1e-6f);                            // count=0 aggregate == 4 equal wheels
  CHECK(std::fabs(aL - aR) < 0.1f * (aL + aR));   // balanced slip stays centred
}

// (4) A gear shift produces a bounded, click-free level excursion, and nothing at
// all while the gear stays unknown (INT_MIN).
void TestShiftFlare() {
  // Synth level: an upshift dips the level, a downshift blips it, both bounded and
  // click-free; with no request the level is flat. Measured as RMS over a window
  // that spans the whole flare (a per-block RMS would just track the ~28 Hz
  // fundamental's phase). High throttle keeps the overrun burble small so the
  // reference window is steady.
  auto flare_window = [](f32 request) {
    SynthVoice voice(kRate, std::unique_ptr<Synth>(new EngineSynth(InlineFourCarPreset(), kRate)));
    SynthParams p;
    p.rpm = 3400.0f;
    p.load = 0.7f;
    p.throttle = 0.9f;
    voice.SetParams(p);
    const u32 win = 9000;  // ~187 ms, a touch longer than the flare envelope
    std::vector<f32> ref(win, 0.0f), flr(win, 0.0f);
    // Warm up, then a steady reference window.
    std::vector<f32> warm(win, 0.0f);
    voice.Read(warm.data(), win);
    voice.Read(ref.data(), win);
    // Trigger the flare (rising edge) and render the flare window contiguously.
    p.gear_shift = request;
    voice.SetParams(p);
    voice.Read(flr.data(), win);
    // Worst sample step across the whole reference+flare span (click check).
    std::vector<f32> both = ref;
    both.insert(both.end(), flr.begin(), flr.end());
    struct R { f32 ref, flr, worst; };
    return R{Rms(ref), Rms(flr), MaxDelta(both)};
  };

  const auto up = flare_window(-1.0f);  // upshift cut
  const auto down = flare_window(1.0f); // downshift blip
  const auto none = flare_window(0.0f); // no request
  std::fprintf(stderr,
               "vehicle_audio_test: shift up(ref=%.4f flr=%.4f worst=%.4f) "
               "down(ref=%.4f flr=%.4f worst=%.4f) none(ref=%.4f flr=%.4f)\n",
               up.ref, up.flr, up.worst, down.ref, down.flr, down.worst, none.ref, none.flr);
  CHECK(up.flr < up.ref * 0.9f);       // upshift audibly cuts the level
  CHECK(up.worst < 0.15f);             // ... without a click
  CHECK(down.flr > down.ref * 1.05f);  // downshift audibly blips it up
  CHECK(down.worst < 0.15f);
  CHECK(std::fabs(none.flr - none.ref) < none.ref * 0.06f);  // flat when idle

  // Mapping level: a gear left INT_MIN must yield no flare even as is_shifting
  // pulses, so the mix is bit-identical to a run that never touches either field.
  auto run_gate = [](bool pulse_shift) {
    Mixer mixer;
    mixer.Configure(kRate);
    VehicleAudio va(mixer, InlineFourCarPreset());
    VehicleAudioState st;
    st.rpm = 3000.0f;
    st.load = 0.5f;
    st.throttle = 0.5f;
    std::vector<f32> out(512 * 2, 0.0f);
    std::vector<f32> acc;
    for (int i = 0; i < 30; ++i) {
      st.is_shifting = pulse_shift && (i == 10);  // pulse, but gear stays INT_MIN
      va.Update(st);
      mixer.MixInto(out.data(), 512);
      acc.insert(acc.end(), out.begin(), out.end());
    }
    return acc;
  };
  const std::vector<f32> plain = run_gate(false);
  const std::vector<f32> pulsed = run_gate(true);
  f32 gate_diff = 0.0f;
  for (size_t i = 0; i < plain.size(); ++i) gate_diff = std::max(gate_diff, std::fabs(plain[i] - pulsed[i]));
  std::fprintf(stderr, "vehicle_audio_test: shift gate (unknown gear) diff=%.6g\n", gate_diff);
  CHECK(gate_diff < 1e-6f);  // unknown gear -> no flare, nothing changes
}

// (5) The jet spool split: N1 pitches the whine, thrust levels the roar. A rising
// N1 at low thrust brightens (whine climbs) without the full roar.
void TestJetSpoolSplit() {
  const EnginePreset jet = LightJetPreset();
  auto render = [&](f32 n1, f32 thrust) {
    SynthParams p;
    p.rpm = n1;         // N1 %
    p.thrust = thrust;  // <0 = derive from rpm
    return RenderEngine(jet, p, 24000);
  };

  // Whine pitch tracks N1 even with thrust pinned low.
  const f32 whine_lo = RawZeroCrossHz(render(40.0f, 0.1f));
  const f32 whine_hi = RawZeroCrossHz(render(85.0f, 0.1f));
  std::fprintf(stderr, "vehicle_audio_test: jet whine Hz n1=40 -> %.1f  n1=85 -> %.1f\n", whine_lo,
               whine_hi);
  CHECK(whine_hi > whine_lo * 1.2f);  // N1 climbs the whine pitch

  // Roar level tracks thrust at fixed N1. The roar is low-frequency broadband
  // noise; a steep (two-pole) low-pass well under the whine isolates it, so the
  // 4 kHz whine tone does not leak in and mask the difference.
  auto roar_rms = [](std::vector<f32> b) {
    LowPass(b, 180.0f);
    LowPass(b, 180.0f);
    return Rms(b);
  };
  const f32 roar_low = roar_rms(render(85.0f, 0.1f));
  const f32 roar_high = roar_rms(render(85.0f, 0.9f));
  std::fprintf(stderr, "vehicle_audio_test: jet roar rms thrust=0.1 -> %.5f thrust=0.9 -> %.5f\n",
               roar_low, roar_high);
  CHECK(roar_high > roar_low * 1.3f);  // thrust drives the roar, spool does not

  // Spool-up character: at high N1, holding thrust low keeps the roar well below
  // the old derive-from-rpm behaviour, while the whine (above) already tracks N1.
  const f32 roar_derive = roar_rms(render(85.0f, -1.0f));
  std::fprintf(stderr, "vehicle_audio_test: jet roar rms derive(n1=85) -> %.5f\n", roar_derive);
  CHECK(roar_low < roar_derive * 0.7f);  // whine climbs before the roar catches up
}

// (6) After the mixer deletes a vehicle's voices (StopAll, then the fade-out
// completes on the device thread), a still-live VehicleAudio must keep updating
// safely. Before the mailbox fix VehicleAudio held raw SynthVoice pointers and
// dereferenced freed memory here (ASan heap-use-after-free); now the parameter
// endpoint outlives the voice through shared ownership, so Update is a no-op on
// the retired voices rather than a fault.
void TestUpdateAfterStopAllIsSafe() {
  Mixer mixer;
  mixer.Configure(kRate);
  VehicleAudio va(mixer, V8Preset());

  VehicleAudioState st;
  st.rpm = 4000.0f;
  st.load = 0.6f;
  st.throttle = 0.7f;
  st.speed_mps = 25.0f;
  st.slip = 0.4f;
  va.Update(st);

  std::vector<f32> out(1024 * 2, 0.0f);
  // Get the voices live in the mixer.
  for (int i = 0; i < 4; ++i) mixer.MixInto(out.data(), 1024);

  // Yank every voice out from under VehicleAudio, then run the mix long enough
  // for the StopAll fade to complete and the mixer to delete each SynthVoice.
  mixer.StopAll();
  for (int i = 0; i < 40; ++i) mixer.MixInto(out.data(), 1024);

  // Keep feeding telemetry: the voices are gone but VehicleAudio does not know.
  // This must not touch freed memory (the pre-fix bug) and must stay finite.
  for (int frame = 0; frame < 200; ++frame) {
    st.rpm = 3000.0f + 40.0f * frame;
    st.slip = 0.5f;
    va.Update(st);
    mixer.MixInto(out.data(), 1024);
    for (f32 v : out) CHECK(std::isfinite(v));
  }
  va.Stop();  // idempotent, also safe after the voices are gone
}

// (7) The parameter hand-off is a legal, wait-free single-producer/single-consumer
// exchange (replacing the old UB seqlock). Hammer it: one thread spins SetParams
// through a real jump range while this thread renders through Read (the device
// path), then confirm the render stayed finite, bounded and click-free the whole
// time. A torn read of the shared struct would surface as a NaN, an out-of-range
// value blowing past the soft limit, or a click.
void TestParamMailboxThreadedHammer() {
  SynthVoice voice(kRate, std::unique_ptr<Synth>(new EngineSynth(InlineFourCarPreset(), kRate)));
  SynthParams warm;
  warm.rpm = 1200.0f;
  warm.load = 0.3f;
  warm.throttle = 0.3f;
  voice.SetParams(warm);

  std::atomic<bool> done{false};
  std::atomic<u64> writes{0};
  // Writer: slam the parameters across the full authored range as fast as it can,
  // so a torn snapshot would mix low-rpm fields with high-rpm fields.
  std::thread writer([&] {
    u32 n = 0;
    while (!done.load(std::memory_order_relaxed)) {
      SynthParams p;
      const f32 phase = static_cast<f32>((n++ % 100)) / 100.0f;
      p.rpm = 800.0f + phase * 6200.0f;
      p.load = phase;
      p.throttle = phase;
      p.speed_mps = phase * 60.0f;
      p.slip = phase;
      p.muffle = (n & 1) ? 1.0f : 0.0f;
      p.gear_shift = (n % 7 == 0) ? 1.0f : 0.0f;
      voice.SetParams(p);
      writes.fetch_add(1, std::memory_order_relaxed);
    }
  });

  const u32 block = 256;
  std::vector<f32> buf(block, 0.0f);
  f32 worst = 0.0f, peak = 0.0f;
  bool finite = true;
  // Bounded, finite render loop on the consumer (device) side.
  for (int blk = 0; blk < 4000; ++blk) {
    voice.Read(buf.data(), block);
    if (!AllFinite(buf)) finite = false;
    worst = std::max(worst, MaxDelta(buf));
    peak = std::max(peak, MaxAbs(buf));
  }
  done.store(true, std::memory_order_relaxed);
  writer.join();

  std::fprintf(stderr,
               "vehicle_audio_test: mailbox hammer writes=%llu peak=%.4f worst-delta=%.4f\n",
               static_cast<unsigned long long>(writes.load()), peak, worst);
  CHECK(finite);           // no torn read ever produced a NaN
  CHECK(peak <= 1.001f);   // no torn read blew past the soft limit
  CHECK(worst < 0.15f);    // the ramp still declicks across every jump
  CHECK(writes.load() > 0);  // the writer actually ran concurrently
}

}  // namespace

int main() {
  TestEngineRendersSound();
  TestRpmRaisesPitch();
  TestLoadChangesTimbre();
  TestSkidGatesOnSlip();
  TestNoClickOnParamJump();
  TestMixerIntegration();
  TestMuffleDarkensAndDucks();
  TestSignedThrottleEquivalence();
  TestPerWheelSlipPansSkid();
  TestShiftFlare();
  TestJetSpoolSplit();
  TestUpdateAfterStopAllIsSafe();
  TestParamMailboxThreadedHammer();
  if (failures == 0) std::printf("vehicle_audio_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
