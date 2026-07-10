#include "anim/rig_player.h"

#include <vector>

#include "anim/anim_internal.h"

namespace rx::anim {

using detail::GraphState;

struct RigPlayer::Impl {
  const GraphState* graph = nullptr;
  kinema::StateMachineInstance sm;
  kinema::PoseArena arena;
  kinema::SyncGroup sync;
  std::vector<f32> params;
  f32 prev_phase = 0.0f;  // last frame's normalized locomotion phase [0,1)
};

RigPlayer::RigPlayer() : impl_(std::make_unique<Impl>()) {}
RigPlayer::~RigPlayer() = default;
RigPlayer::RigPlayer(RigPlayer&&) noexcept = default;
RigPlayer& RigPlayer::operator=(RigPlayer&&) noexcept = default;

void RigPlayer::Bind(const AnimGraph& graph) {
  const GraphState* g = graph.state();
  Impl& d = *impl_;
  d.graph = g;
  if (!g) return;
  d.params.assign(g->param_names.size(), 0.0f);
  d.arena.Init(g->skeleton.count(), g->machine.max_registers());
  d.sm.Init(g->machine, g->idle_state);
  // Foot-sync the two moving gaits on their footfall markers (their events).
  d.sync.Clear();
  if (const kinema::Clip* w = g->clip(g->walk_clip)) d.sync.AddClip(*w);
  if (const kinema::Clip* r = g->clip(g->run_clip)) d.sync.AddClip(*r);
  d.sync.Reset(0.0f);
  d.prev_phase = 0.0f;
}

bool RigPlayer::bound() const { return impl_->graph != nullptr; }

void RigPlayer::SetSpeed(f32 planar_mps) {
  const GraphState* g = impl_->graph;
  if (g && g->speed_param >= 0) SetParam(g->speed_param, planar_mps);
}

void RigPlayer::SetParam(int index, f32 value) {
  if (index >= 0 && static_cast<size_t>(index) < impl_->params.size())
    impl_->params[static_cast<size_t>(index)] = value;
}

void RigPlayer::Trigger(u32 id) { impl_->sm.SetTrigger(static_cast<kinema::u16>(id)); }

u32 RigPlayer::state() const { return impl_->sm.state(); }

Vec3 RigPlayer::Update(f32 dt, SkeletonPose* out, const EventSink& on_event) {
  Impl& d = *impl_;
  const GraphState& g = *d.graph;
  const u32 n = g.skeleton.count();
  if (out->size() != n) {
    out->translation.resize(n);
    out->rotation.resize(n);
    out->scale.resize(n);
  }

  // Drive the foot-sync phase at a cadence proportional to commanded speed, so
  // walk and run stay foot-aligned and the gait quickens as the actor speeds up.
  const f32 speed = (g.speed_param >= 0) ? d.params[static_cast<size_t>(g.speed_param)] : 0.0f;
  const f32 play_rate = (speed > 0.05f ? speed : 0.0f) / g.walk_speed;
  d.sync.Advance(dt, /*leader=*/0, play_rate);
  const u32 markers = d.sync.marker_count();
  const f32 phase = markers > 0 ? d.sync.phase() / static_cast<f32>(markers) : 0.0f;
  if (g.phase_param >= 0) d.params[static_cast<size_t>(g.phase_param)] = phase;

  // Evaluate the graph (one inertialized program) straight into the caller's
  // SoA pose - the kinema view aliases it, no copy.
  kinema::PoseParams pp{d.params.data(), static_cast<u32>(d.params.size())};
  kinema::PoseView pv = detail::AsKinema(*out);
  d.sm.Update(dt, pp, d.arena, pv, nullptr);

  // Root motion + notifies come from the active locomotion clip swept over the
  // foot-synced phase, so both track the visible gait. Idle contributes neither.
  Vec3 root{};
  if (d.sm.state() == g.loco_state) {
    const kinema::Clip* c = g.clip(g.walk_clip);
    const f32 dur = c->duration();
    f32 dphase = phase - d.prev_phase;
    if (dphase < 0) dphase += 1.0f;  // wrapped this frame
    root = detail::ToRx(c->RootDeltaLooped(d.prev_phase * dur, dphase * dur));
    if (on_event) {
      const f32 t0 = d.prev_phase * dur;
      const f32 t1 = phase * dur;
      c->EventsInRange(t0, t1, [&](const kinema::ClipEvent& e) {
        on_event(Event{e.name_hash, e.name ? std::string_view(e.name) : std::string_view{},
                       Event::Phase::kPoint});
      });
      c->RangedEventsInRange(t0, t1, [&](const kinema::ClipRangedEvent& r, kinema::RangePhase p) {
        Event::Phase ph = p == kinema::RangePhase::kEnter    ? Event::Phase::kEnter
                          : p == kinema::RangePhase::kActive ? Event::Phase::kActive
                                                             : Event::Phase::kExit;
        on_event(Event{r.name_hash, r.name ? std::string_view(r.name) : std::string_view{}, ph});
      });
    }
  }
  d.prev_phase = phase;
  return root;
}

f32 RigPlayer::SampleCurve(u64 name_hash, f32 fallback) const {
  const GraphState& g = *impl_->graph;
  const kinema::Clip* c = g.clip(g.walk_clip);
  if (!c) return fallback;
  return c->SampleCurve(name_hash, impl_->prev_phase * c->duration(), fallback);
}

}  // namespace rx::anim
