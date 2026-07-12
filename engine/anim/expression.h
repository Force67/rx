#ifndef RX_ANIM_EXPRESSION_H_
#define RX_ANIM_EXPRESSION_H_

#include <initializer_list>
#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::anim {

// Facial expression controller over morph-target weights. Poses are data - a
// named set of (target name, weight) pairs - and switching poses never snaps
// or lerps: every channel runs a critically-damped spring toward its goal, so
// transitions are C1-continuous and a retarget mid-flight carries the current
// velocity instead of popping (the morph-weight analogue of kinema's
// inertialized state switches). Channels are classified into facial regions
// by target-name prefix, each with its own response and onset delay, so the
// eyes and brows lead a new expression and the mouth and jaw settle in a beat
// behind them, the way real faces move.
//
// An always-on life layer keeps a held expression from going dead: periodic
// blinks at randomized intervals (fast close, slower open, occasional double
// blink) and low-amplitude smoothed noise on the brows. Blinks composite over
// the expression with max(), so a pose that already holds the eyes closed
// absorbs them, and the micro-motion fades out as the expression takes over
// its channels. The layer is seedable and the whole controller is
// deterministic for a fixed seed and dt sequence.
//
// The controller is one producer of morph weights and does not know about
// imported glTF weight tracks. When a mesh has an active imported animation
// that track wins: drive the instance from the track and leave the controller
// out rather than mixing the two (the viewer follows this rule).
//
// Output weights are final and clamped to [0, 1]; write them into the dense
// per-target set and feed FrameView::morph_weights through the usual
// AppendActiveMorphWeights path.
class RX_ANIM_EXPORT ExpressionController {
 public:
  // One channel of a pose: a morph target by source name (e.g. "jawOpen"),
  // hashed with MakeAssetId to match asset::MorphTarget::name_hash.
  struct PoseEntry {
    std::string_view target;
    f32 weight = 0;
  };

  // Response of one facial region. Channels take the first rule whose prefix
  // starts the target name (an empty prefix is the fallback). `halflife` is
  // the spring halflife toward a new goal; `delay` holds the previous goal
  // for that long after SetExpression, staggering the region behind faster
  // ones.
  struct Region {
    std::string prefix;
    f32 halflife = 0.1f;  // seconds
    f32 delay = 0;        // seconds
  };

  // Life-layer tuning. Blinks drive eyeBlinkLeft/Right; micro-motion rides
  // the brow channels.
  struct LifeConfig {
    bool enabled = true;
    f32 blink_interval_min = 2.0f;  // seconds between blinks
    f32 blink_interval_max = 6.0f;
    f32 double_blink_chance = 0.25f;  // immediate second blink
    f32 blink_close = 0.07f;          // lid close time (fast)
    f32 blink_hold = 0.04f;
    f32 blink_open = 0.17f;  // lid open time (slower)
    f32 micro_amplitude = 0.03f;
    f32 micro_hz = 0.5f;  // brow noise knot frequency
  };

  ExpressionController();

  // Pose library. AddPose replaces a same-named pose and creates channels for
  // targets no earlier pose mentioned. AddDefaultPoses ships the stock set
  // (neutral, smile, angry, surprised, eyes_closed, pucker, smirk), built for
  // ARKit-style target names.
  void AddPose(std::string_view name, const PoseEntry* entries, u32 count);
  void AddPose(std::string_view name, std::initializer_list<PoseEntry> entries) {
    AddPose(name, entries.begin(), static_cast<u32>(entries.size()));
  }
  void AddDefaultPoses();

  // Replace the region table. Applies to channels created by later AddPose
  // calls; the defaults respond eye/brow fastest, then nose/cheek, then
  // mouth/jaw trailing by tens of milliseconds.
  void SetRegions(const Region* regions, u32 count);

  // Start transitioning to a pose; channels the pose leaves out ease back to
  // zero. `transition_time` scales the per-region response and delay (an
  // approximate settle time; <= 0 keeps the defaults, which settle in about
  // 0.4 s). Returns false when no such pose was added.
  bool SetExpression(std::string_view name, f32 transition_time = 0);
  bool SetExpression(u64 pose_hash, f32 transition_time = 0);
  u64 expression() const { return active_pose_; }

  // Reseeds the life layer and restarts its timers (deterministic replays).
  void set_seed(u64 seed);
  void set_life(const LifeConfig& config) { life_ = config; }
  const LifeConfig& life() const { return life_; }

  void Update(f32 dt);

  // Smoothed output, one channel per target the pose library ever mentioned
  // (plus the life layer's blink/brow channels). Targets are
  // MakeAssetId(name).hash; weights are composited and clamped to [0, 1].
  u32 channel_count() const { return static_cast<u32>(channels_.size()); }
  u64 channel_target(u32 index) const { return channels_[index].target; }
  f32 channel_weight(u32 index) const { return channels_[index].out; }
  f32 Weight(u64 target_hash) const;

 private:
  struct Channel {
    std::string name;
    u64 target = 0;
    f32 value = 0;
    f32 velocity = 0;
    f32 goal = 0;
    f32 pending_goal = 0;
    f32 pending = -1;  // seconds until pending_goal takes over; < 0 = none
    f32 halflife = 0.1f;   // active response (region halflife, scaled)
    f32 base_halflife = 0.1f;
    f32 base_delay = 0;
    f32 out = 0;
    i32 micro = -1;  // life-layer noise lane, -1 = none
    bool blink = false;
  };
  struct Pose {
    u64 name_hash = 0;
    base::Vector<u32> channel;
    base::Vector<f32> weight;
  };
  enum class BlinkPhase : u8 { kWait, kClose, kHold, kOpen };

  u32 EnsureChannel(std::string_view name);
  void UpdateBlink(f32 dt);
  f32 NextRand01();

  base::Vector<Region> regions_;
  base::Vector<Channel> channels_;
  base::Vector<Pose> poses_;
  base::Vector<f32> scratch_;
  u64 active_pose_ = 0;

  LifeConfig life_;
  u64 seed_ = 0;
  u64 rng_ = 0;
  f32 life_time_ = 0;
  BlinkPhase blink_phase_ = BlinkPhase::kWait;
  f32 blink_t_ = 0;
  f32 blink_wait_ = 0;
  f32 blink_env_ = 0;
  bool blink_double_ = false;  // current wait is the gap of a double blink
};

}  // namespace rx::anim

#endif  // RX_ANIM_EXPRESSION_H_
