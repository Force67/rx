#include "audio/vehicle_audio.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "audio/aux_synth.h"
#include "audio/mixer.h"

namespace rx::audio {
namespace {

// Per-layer mix levels at full presence. The engine carries the sound; skid and
// wind sit under it so they season rather than mask.
constexpr f32 kEngineGain = 0.9f;
constexpr f32 kSkidGain = 0.7f;
constexpr f32 kWindGain = 0.55f;

// Submerged duck: the engine goes distant and muffled, and there is no tyre
// squeal or wind rush underwater at all.
constexpr f32 kSubmergedEngineGain = 0.35f;

// A gain command is only re-sent when it moves more than this, to keep a moving
// vehicle from flooding the command queue every frame.
constexpr f32 kGainEpsilon = 0.01f;
// Likewise for position, in metres.
constexpr f32 kPositionEpsilon = 0.05f;

// Vehicles carry a fair way; the engine reaches furthest.
Attenuation EngineAtten() { return Attenuation{6.0f, 130.0f}; }
Attenuation AuxAtten() { return Attenuation{5.0f, 80.0f}; }

f32 Dist2(const Vec3& a, const Vec3& b) {
  const Vec3 d = a - b;
  return d.x * d.x + d.y * d.y + d.z * d.z;
}

}  // namespace

VehicleAudio::VehicleAudio(Mixer& mixer, const EnginePreset& preset) : mixer_(&mixer) {
  const u32 rate = mixer.output_rate();

  auto start = [&](Layer& layer, std::unique_ptr<Synth> model, const Attenuation& atten) {
    auto voice = std::make_unique<SynthVoice>(rate, std::move(model));
    layer.synth = voice.get();  // valid until the mixer retires the voice
    PlayParams params;
    params.positional = true;
    params.gain = 0.0f;  // rise in from silence as Update feeds telemetry
    params.atten = atten;
    layer.voice = mixer.Play(std::move(voice), params);
    layer.sent_gain = 0.0f;
  };

  start(engine_, std::make_unique<EngineSynth>(preset, rate), EngineAtten());
  start(skid_, std::make_unique<SkidSynth>(rate), AuxAtten());
  start(wind_, std::make_unique<WindSynth>(rate), AuxAtten());
}

VehicleAudio::~VehicleAudio() { Stop(); }

void VehicleAudio::SetLayerGain(Layer& layer, f32 gain) {
  if (!layer.voice) return;
  if (std::abs(gain - layer.sent_gain) < kGainEpsilon) return;
  mixer_->SetVoiceGain(layer.voice, gain);
  layer.sent_gain = gain;
}

void VehicleAudio::Update(const VehicleAudioState& state) {
  if (stopped_ || !mixer_) return;

  // Telemetry -> synth parameters. Each layer reads only what it needs, but the
  // block is shared so there is one contract to fill.
  SynthParams params;
  params.rpm = state.rpm;
  params.load = state.load;
  params.throttle = state.throttle;
  params.speed_mps = state.speed_mps;
  params.slip = state.slip;
  if (engine_.synth) engine_.synth->SetParams(params);
  if (skid_.synth) skid_.synth->SetParams(params);
  if (wind_.synth) wind_.synth->SetParams(params);

  // Gains, including the underwater duck. The synth voices smooth their own
  // parameters; the mixer ramps gain, so these need no smoothing here.
  const f32 engine_gain = state.submerged ? kSubmergedEngineGain : kEngineGain;
  const f32 aux_scale = state.submerged ? 0.0f : 1.0f;
  SetLayerGain(engine_, engine_gain);
  SetLayerGain(skid_, kSkidGain * aux_scale);
  SetLayerGain(wind_, kWindGain * aux_scale);

  // Placement: one position drives all three voices. Only resend when it moved,
  // so a parked vehicle issues no per-frame commands.
  if (!have_position_ || Dist2(state.position, last_position_) > kPositionEpsilon * kPositionEpsilon) {
    if (engine_.voice) mixer_->SetVoicePosition(engine_.voice, state.position);
    if (skid_.voice) mixer_->SetVoicePosition(skid_.voice, state.position);
    if (wind_.voice) mixer_->SetVoicePosition(wind_.voice, state.position);
    last_position_ = state.position;
    have_position_ = true;
  }
}

void VehicleAudio::Stop() {
  if (stopped_ || !mixer_) return;
  // Short fade so the loops do not click off. The mixer frees the decoders when
  // the fade completes; the raw synth pointers must not be touched afterwards.
  if (engine_.voice) mixer_->Stop(engine_.voice, 0.08f);
  if (skid_.voice) mixer_->Stop(skid_.voice, 0.08f);
  if (wind_.voice) mixer_->Stop(wind_.voice, 0.08f);
  engine_.synth = skid_.synth = wind_.synth = nullptr;
  engine_.voice = skid_.voice = wind_.voice = 0;
  stopped_ = true;
}

}  // namespace rx::audio
