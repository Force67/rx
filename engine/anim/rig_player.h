#ifndef RX_ANIM_RIG_PLAYER_H_
#define RX_ANIM_RIG_PLAYER_H_

#include <functional>
#include <memory>
#include <string_view>

#include "anim/anim_graph.h"
#include "anim/pose.h"
#include "core/math.h"

namespace rx::anim {

// Plays one character's animation graph. It binds to a shared AnimGraph
// (archetype) and owns the per-character state that graph needs: a state-machine
// instance, a foot-sync phase, the live parameter block and the fixed-capacity
// pose arena. Update advances the graph one frame, writes the local pose and
// returns the frame's root-motion delta; the caller owns moving the entity.
//
// No heap allocation happens after Bind: the arena and buffers are sized once,
// and the SoA pose stays SoA until the caller skins it.
class RigPlayer {
 public:
  RigPlayer();
  ~RigPlayer();
  RigPlayer(RigPlayer&&) noexcept;
  RigPlayer& operator=(RigPlayer&&) noexcept;
  RigPlayer(const RigPlayer&) = delete;
  RigPlayer& operator=(const RigPlayer&) = delete;

  // Size the per-character buffers to the archetype and start in its first
  // state. The AnimGraph must outlive the player (it is shared, held by value).
  void Bind(const AnimGraph& graph);
  bool bound() const;

  // Live inputs read by the graph each frame. SetSpeed is the locomotion
  // convenience (the "speed" parameter); SetParam takes an AnimGraph::ParamIndex.
  void SetSpeed(f32 planar_mps);
  void SetParam(int index, f32 value);
  void Trigger(u32 id);  // raise an edge trigger a transition may consume

  // An animation notify surfaced to the caller (a footfall point marker, or the
  // enter/active/exit edge of a contact range). `name` points into the clip blob
  // and is valid for the duration of the callback only.
  struct Event {
    u64 name_hash = 0;
    std::string_view name;
    enum class Phase : u8 { kPoint, kEnter, kActive, kExit } phase = Phase::kPoint;
  };
  using EventSink = std::function<void(const Event&)>;

  // Advance the graph by dt, write this frame's local pose into `out` (sized to
  // the skeleton) and return the model-space root-translation delta for this
  // frame. Fired notifies (footfalls / contact ranges) are reported through
  // `on_event`, matched to the visible gait phase.
  Vec3 Update(f32 dt, SkeletonPose* out, const EventSink& on_event = {});

  u32 state() const;
  // Sample a named float curve of the active locomotion clip at the current
  // phase (e.g. the footstep-intensity curve), or `fallback` if absent.
  f32 SampleCurve(u64 name_hash, f32 fallback = 0.0f) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rx::anim

#endif  // RX_ANIM_RIG_PLAYER_H_
