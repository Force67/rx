#ifndef RX_AUDIO_VEHICLE_AUDIO_H_
#define RX_AUDIO_VEHICLE_AUDIO_H_

#include "audio/engine_synth.h"
#include "audio/synth_voice.h"
#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::audio {

class Mixer;

// Everything the game layer feeds one vehicle's audio each frame. Plain data,
// no engine/physics types: the physics or demo layer fills it from drivetrain
// telemetry. Values are clamped on use, so a rough estimate is fine.
struct VehicleAudioState {
  f32 rpm = 0.0f;        // crank rpm (turbine preset: N1 %, 0..100)
  f32 load = 0.0f;       // 0..1 engine load (torque demand met)
  f32 throttle = 0.0f;   // 0..1 driver throttle input
  f32 speed_mps = 0.0f;  // ground speed, metres/second
  f32 slip = 0.0f;       // 0..1 aggregate tyre slip ratio
  bool submerged = false;  // vehicle is underwater: duck and muffle
  Vec3 position{};         // world position (Y-up, metres) for panning
};

// Per-vehicle procedural audio: owns the engine, skid and wind voices and drives
// them from telemetry. This is the only audio class the game layer needs for a
// vehicle: construct it with a preset, call Update once per frame, and Stop (or
// let it destruct) to retire the voices. All three voices are positional, so the
// mixer's spatial path places them; no Doppler is done here.
class RX_AUDIO_EXPORT VehicleAudio {
 public:
  // Starts the three voices on `mixer` at the mixer's output rate. Voices begin
  // near-silent and rise as Update feeds them; safe on a mixer with no device
  // (the commands simply queue until something drains them).
  VehicleAudio(Mixer& mixer, const EnginePreset& preset);
  ~VehicleAudio();

  VehicleAudio(const VehicleAudio&) = delete;
  VehicleAudio& operator=(const VehicleAudio&) = delete;

  // Pushes one frame of telemetry to the synth voices and updates placement and
  // ducking. Cheap: it publishes parameters lock-free and only issues a mixer
  // command when a gain or the position actually changed.
  void Update(const VehicleAudioState& state);

  // Fades the voices out and releases them. Idempotent; also called by the dtor.
  void Stop();

 private:
  struct Layer {
    u32 voice = 0;         // mixer voice id (0 = not started)
    SynthVoice* synth = nullptr;  // non-owning: the mixer owns the decoder
    f32 sent_gain = -1.0f;        // last gain pushed, to suppress no-op commands
  };

  void SetLayerGain(Layer& layer, f32 gain);

  Mixer* mixer_ = nullptr;
  Layer engine_;
  Layer skid_;
  Layer wind_;
  Vec3 last_position_{};
  bool have_position_ = false;
  bool stopped_ = false;
};

}  // namespace rx::audio

#endif  // RX_AUDIO_VEHICLE_AUDIO_H_
