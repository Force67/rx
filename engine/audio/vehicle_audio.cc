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

// How far the skid voice is nudged sideways at a full one-side slip, in metres.
// Small on purpose: it only tilts the existing distance pan toward the slipping
// side, it must not move the source enough to change its loudness.
constexpr f32 kSkidPanMeters = 1.5f;

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

  // Per-wheel slip -> aggregate intensity plus skid pan/shade biases. With no
  // per-wheel data (wheel_count == 0, the default) this uses the aggregate `slip`
  // field exactly as before, so existing callers are unchanged.
  f32 slip = state.slip;
  f32 skid_bias = 0.0f;  // -1 rear .. +1 front, shades the skid band
  f32 lat_bias = 0.0f;   // -1 left .. +1 right, pans the skid voice sideways
  if (state.wheel_count > 0) {
    const u32 n = std::min<u32>(state.wheel_count, 4u);
    f32 mx = 0.0f;
    for (u32 w = 0; w < n; ++w) mx = std::max(mx, state.wheel_slip[w]);
    slip = mx;  // intensity from the worst-slipping wheel
    if (n >= 4) {
      // Order is FL FR RL RR. Bias by which side / axle washes more.
      const f32 left = state.wheel_slip[0] + state.wheel_slip[2];
      const f32 right = state.wheel_slip[1] + state.wheel_slip[3];
      const f32 front = state.wheel_slip[0] + state.wheel_slip[1];
      const f32 rear = state.wheel_slip[2] + state.wheel_slip[3];
      lat_bias = (right - left) / (right + left + 1e-4f);
      skid_bias = (front - rear) / (front + rear + 1e-4f);
    }
  }

  // Gear-shift flare direction. INT_MIN gear is "unknown" -> never flare, so a
  // caller that leaves gear/is_shifting untouched hears no change. A gear change
  // gives the authoritative direction; the rising edge of is_shifting also fires
  // one (defaulting to an upshift cut when the gear delta is not yet visible).
  f32 gear_shift = 0.0f;
  if (state.gear != INT_MIN) {
    const bool known_prev = last_gear_ != INT_MIN;
    const bool gear_changed = known_prev && state.gear != last_gear_;
    const bool shift_edge = state.is_shifting && !prev_shifting_;
    if (gear_changed || shift_edge) {
      const bool downshift = known_prev && state.gear < last_gear_;
      gear_shift = downshift ? 1.0f : -1.0f;  // >0 blip, <0 cut
    }
  }
  prev_shifting_ = state.is_shifting;
  if (state.gear != INT_MIN) last_gear_ = state.gear;

  // Telemetry -> synth parameters. Each layer reads only what it needs, but the
  // block is shared so there is one contract to fill.
  SynthParams params;
  params.rpm = state.rpm;
  params.load = state.load;
  params.throttle = std::fabs(state.throttle);  // signed input (astern); use its magnitude
  params.speed_mps = state.speed_mps;
  params.slip = slip;
  params.muffle = state.submerged ? 1.0f : 0.0f;  // synth-side dark/duck (smoothed)
  params.thrust = state.thrust;
  params.gear_shift = gear_shift;
  params.skid_bias = skid_bias;
  if (engine_.synth) engine_.synth->SetParams(params);
  if (skid_.synth) skid_.synth->SetParams(params);
  if (wind_.synth) wind_.synth->SetParams(params);

  // Gains, including the underwater duck. The synth voices smooth their own
  // parameters; the mixer ramps gain, so these need no smoothing here. The
  // engine's submerged duck is kept alongside the synth-side muffle above (the
  // muffle darkens timbre, the gain pushes it distant).
  const f32 engine_gain = state.submerged ? kSubmergedEngineGain : kEngineGain;
  const f32 aux_scale = state.submerged ? 0.0f : 1.0f;
  SetLayerGain(engine_, engine_gain);
  SetLayerGain(skid_, kSkidGain * aux_scale);
  SetLayerGain(wind_, kWindGain * aux_scale);

  // Placement. Engine and wind sit at the vehicle; the skid is nudged toward the
  // slipping side so a one-wheel slide pans a touch off-centre. Absent a body
  // orientation in the state, the lateral bias is applied in world X: the engine
  // convention is +Z forward, so the right side is -X and a right-biased slip
  // (+lat_bias) offsets toward -X. This is exact for an unrotated body; a caller
  // wanting yaw-accurate panning can bake its rotation into `position`. Each voice
  // only resends when it actually moved, so a parked vehicle issues no commands.
  if (!have_position_ || Dist2(state.position, last_position_) > kPositionEpsilon * kPositionEpsilon) {
    if (engine_.voice) mixer_->SetVoicePosition(engine_.voice, state.position);
    if (wind_.voice) mixer_->SetVoicePosition(wind_.voice, state.position);
    last_position_ = state.position;
  }
  Vec3 skid_pos = state.position;
  skid_pos.x -= lat_bias * kSkidPanMeters;
  if (!have_position_ ||
      Dist2(skid_pos, last_skid_position_) > kPositionEpsilon * kPositionEpsilon) {
    if (skid_.voice) mixer_->SetVoicePosition(skid_.voice, skid_pos);
    last_skid_position_ = skid_pos;
  }
  have_position_ = true;
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
