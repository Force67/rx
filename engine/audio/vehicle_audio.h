#ifndef RX_AUDIO_VEHICLE_AUDIO_H_
#define RX_AUDIO_VEHICLE_AUDIO_H_

#include <climits>
#include <memory>

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
  f32 throttle = 0.0f;   // driver throttle; -1..1 (boats run astern), magnitude used
  f32 speed_mps = 0.0f;  // ground speed, metres/second
  f32 slip = 0.0f;       // 0..1 aggregate tyre slip ratio (fallback / no per-wheel)
  bool submerged = false;  // vehicle is underwater: duck and muffle
  Vec3 position{};         // world position (Y-up, metres) for panning

  // --- additive, default-inert fields (vehicle-realism pass) -----------------
  // Optional per-wheel slip, order FL FR RL RR (+Z forward, right side is -X).
  // `wheel_count` 0 (the default) ignores `wheel_slip` and uses the aggregate
  // `slip` above exactly as before; >=1 takes intensity from the worst wheel, and
  // a full set (>=4) also pans the skid toward the slipping side and shades its
  // band by which axle washes.
  f32 wheel_slip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  u32 wheel_count = 0;

  // Gearbox state for the shift flare. `gear` == INT_MIN (the default) means
  // "unknown": no flare is ever produced, so existing callers hear no change. A
  // rising edge of `is_shifting` with a known gear triggers one brief, click-free
  // engine excursion — an upshift cut if the gear rose, a downshift blip if it
  // fell.
  i32 gear = INT_MIN;
  bool is_shifting = false;

  // Turbine roar level, 0..1, decoupled from spool (jets). <0 (the default)
  // derives the roar from rpm/N1 as before; >=0 lets the roar lag the whine.
  f32 thrust = -1.0f;
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
    u32 voice = 0;  // mixer voice id (0 = not started)
    // Parameter endpoint, shared with the SynthVoice. Held by shared_ptr rather
    // than a raw voice pointer so publishing survives the mixer retiring and
    // deleting the voice: a late Update lands in a still-live mailbox, not freed
    // memory.
    std::shared_ptr<ParamMailbox> params;
    f32 sent_gain = -1.0f;  // last gain pushed, to suppress no-op commands
  };

  void SetLayerGain(Layer& layer, f32 gain);

  Mixer* mixer_ = nullptr;
  Layer engine_;
  Layer skid_;
  Layer wind_;
  Vec3 last_position_{};
  Vec3 last_skid_position_{};  // skid carries a lateral pan bias, so it moves apart
  bool have_position_ = false;
  bool stopped_ = false;

  // Shift-flare edge detection: fire once on a rising `is_shifting` with a known
  // gear, using the gear delta for the up/down direction.
  i32 last_gear_ = INT_MIN;
  bool prev_shifting_ = false;
};

}  // namespace rx::audio

#endif  // RX_AUDIO_VEHICLE_AUDIO_H_
