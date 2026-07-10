#ifndef RX_ANIM_ANIM_GRAPH_H_
#define RX_ANIM_ANIM_GRAPH_H_

#include <memory>
#include <string_view>

#include "asset/skeleton.h"
#include "core/types.h"

namespace rx::anim {

namespace detail {
struct GraphState;
}

// The shared, immutable animation archetype: a skeleton binding plus a compiled
// state graph (states, transitions, blend spaces and the clips they sample),
// built once and shared by every character that plays it. Carries no per-frame
// state - that lives in RigPlayer. Copies are cheap (a shared_ptr to the
// compiled payload), so a whole crowd of RigPlayers shares one AnimGraph.
//
// The graph is authored kinema-side (kinema stays a private detail of
// engine/anim, so it never appears in this header); BuildBipedLocomotionGraph is
// the first such archetype factory.
class AnimGraph {
 public:
  AnimGraph() = default;
  explicit AnimGraph(std::shared_ptr<detail::GraphState> state) : state_(std::move(state)) {}

  bool valid() const { return static_cast<bool>(state_); }
  u32 bone_count() const;

  // Parameter id for a named live input (e.g. "speed"), or -1 if the archetype
  // has no such parameter. RigPlayer::SetParam takes this id.
  int ParamIndex(std::string_view name) const;

  // Internal handle to the compiled payload. Returns a detail type, so it is
  // only usable in a translation unit that includes anim_internal.h (RigPlayer /
  // FootPlacement); ordinary callers cannot reach kinema through it.
  const detail::GraphState* state() const { return state_.get(); }

 private:
  std::shared_ptr<detail::GraphState> state_;
};

// Author an idle / walk / run locomotion archetype for a biped built on the
// "NPC ... [ ]" rig bone convention (see asset::MakeSkinnedBiped). It bakes
// three procedural clips (an idle sway, a walk cycle and a run cycle, the two
// gaits carrying matched footfall markers, a footstep-intensity curve and
// forward root motion), a 1D walk<->run blend space on a "speed" parameter, and
// a two-state machine (idle <-> locomotion) whose transitions are driven by
// "speed". The result is shared by every RigPlayer of this rig.
AnimGraph BuildBipedLocomotionGraph(const asset::Skeleton& skeleton);

}  // namespace rx::anim

#endif  // RX_ANIM_ANIM_GRAPH_H_
