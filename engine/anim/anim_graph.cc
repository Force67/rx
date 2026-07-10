#include "anim/anim_graph.h"

#include <cmath>

#include "anim/anim_internal.h"
#include "core/math.h"

namespace rx::anim {

namespace detail {

kinema::Skeleton BuildKinemaSkeleton(const asset::Skeleton& skeleton) {
  kinema::Skeleton out;
  const u32 n = static_cast<u32>(skeleton.bones.size());
  out.parents.resize(n);
  out.name_hashes.resize(n);
  out.bind_translation.resize(n);
  out.bind_rotation.resize(n);
  out.bind_scale.resize(n);
  for (u32 i = 0; i < n; ++i) {
    const asset::Bone& b = skeleton.bones[i];
    out.parents[i] = static_cast<kinema::i16>(b.parent);
    out.name_hashes[i] = kinema::HashName(b.name);
    out.bind_translation[i] = kinema::Vec3{b.bind_translation.x, b.bind_translation.y,
                                           b.bind_translation.z};
    out.bind_rotation[i] =
        kinema::Quat{b.bind_rotation.x, b.bind_rotation.y, b.bind_rotation.z, b.bind_rotation.w};
    out.bind_scale[i] = b.bind_scale;
  }
  return out;
}

}  // namespace detail

namespace {

using detail::GraphState;

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 2.0f * kPi;

f32 Clamp01(f32 v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// bind * axis-angle(axis, rad), staying in kinema's quaternion type.
kinema::Quat Compose(const kinema::Quat& bind, const Vec3& axis, f32 rad) {
  Quat s = QuatFromAxisAngle(axis, rad);
  Quat b{bind.x, bind.y, bind.z, bind.w};
  Quat r = b * s;
  return kinema::Quat{r.x, r.y, r.z, r.w};
}

// Named driven joints of the biped rig (kFoot/kThigh/... names live in
// asset::MakeSkinnedBiped). -1 for a joint the skeleton lacks.
struct GaitJoints {
  int l_thigh, r_thigh, l_calf, r_calf, l_arm, r_arm, spine, spine1;
  static GaitJoints Resolve(const kinema::Skeleton& sk) {
    auto f = [&](const char* n) { return sk.Find(kinema::HashName(n)); };
    return GaitJoints{f("NPC L Thigh [LThg]"), f("NPC R Thigh [RThg]"),
                      f("NPC L Calf [LClf]"),  f("NPC R Calf [RClf]"),
                      f("NPC L UpperArm [LUar]"), f("NPC R UpperArm [RUar]"),
                      f("NPC Spine [Spn0]"),   f("NPC Spine1 [Spn1]")};
  }
};

struct GaitAmp {
  f32 thigh, knee, arm, spine, lean;
};

// Bake one procedural gait clip: a full local pose per frame (bind for every
// untouched bone, X-axis swings on the legs/arms and a Z-axis spine sway on the
// driven ones), matched footfall markers + contact ranges + a footstep-intensity
// curve on the two moving gaits, and forward root motion. Seamlessly loops
// (frame `frames-1` reproduces frame 0).
kinema::OwnedClip BakeGait(const kinema::Skeleton& sk, u32 frames, f32 rate, const GaitAmp& amp,
                           f32 stride, bool footfalls) {
  const u32 bones = sk.count();
  const f32 duration = static_cast<f32>(frames - 1) / rate;
  const GaitJoints j = GaitJoints::Resolve(sk);
  kinema::ClipBuilder b(bones, frames, rate);

  int curve = -1;
  if (footfalls) {
    b.AddEvent("FootLeft", 0.25f * duration);
    b.AddEvent("FootRight", 0.75f * duration);
    b.AddRangedEvent("ContactLeft", 0.18f * duration, 0.34f * duration);
    b.AddRangedEvent("ContactRight", 0.68f * duration, 0.84f * duration);
    curve = b.AddCurve("FootstepIntensity");
  }
  if (stride != 0.0f) {
    b.AddRootKey(0.0f, kinema::Vec3{0, 0, 0});
    b.AddRootKey(duration, kinema::Vec3{0, 0, stride});
  }

  const Vec3 x{1, 0, 0};
  const Vec3 z{0, 0, 1};
  for (u32 fr = 0; fr < frames; ++fr) {
    const f32 phase = static_cast<f32>(fr) / static_cast<f32>(frames - 1);  // 0..1, wraps
    const f32 theta = phase * kTwoPi;
    const f32 leg = std::sin(theta);

    for (u32 bone = 0; bone < bones; ++bone) {
      kinema::Quat rot = sk.bind_rotation[bone];
      auto is = [&](int idx) { return idx >= 0 && static_cast<u32>(idx) == bone; };
      if (is(j.l_thigh)) rot = Compose(rot, x, amp.thigh * leg);
      else if (is(j.r_thigh)) rot = Compose(rot, x, -amp.thigh * leg);
      else if (is(j.l_calf))
        rot = Compose(rot, x, -amp.knee * Clamp01(-std::sin(theta - 0.6f)));
      else if (is(j.r_calf))
        rot = Compose(rot, x, -amp.knee * Clamp01(-std::sin(theta + kPi - 0.6f)));
      else if (is(j.l_arm)) rot = Compose(rot, x, -amp.arm * leg);
      else if (is(j.r_arm)) rot = Compose(rot, x, amp.arm * leg);
      else if (is(j.spine1)) rot = Compose(rot, z, amp.spine * std::cos(theta));
      else if (is(j.spine)) rot = Compose(rot, x, amp.lean);  // constant forward lean
      b.SetSample(fr, bone, sk.bind_translation[bone], rot, sk.bind_scale[bone]);
    }
    if (curve >= 0) {
      // Two intensity pulses per cycle, peaking at the footfalls.
      f32 pL = std::exp(-std::pow((phase - 0.25f) * 6.0f, 2.0f));
      f32 pR = std::exp(-std::pow((phase - 0.75f) * 6.0f, 2.0f));
      b.SetCurveSample(fr, static_cast<u32>(curve), std::max(pL, pR));
    }
  }
  return kinema::OwnedClip(b.Build());
}

}  // namespace

AnimGraph BuildBipedLocomotionGraph(const asset::Skeleton& skeleton) {
  auto state = std::make_shared<GraphState>();
  state->skeleton = detail::BuildKinemaSkeleton(skeleton);
  const kinema::Skeleton& sk = state->skeleton;

  state->param_names = {"speed", "phase"};
  state->speed_param = 0;
  state->phase_param = 1;
  state->footstep_curve = kinema::HashName("FootstepIntensity");

  // Three procedurally-authored clips that visibly differ: a slow 2 s idle sway,
  // a 1 s walk cycle and a punchier 0.6 s run cycle. Walk and run carry matched
  // footfall markers so the sync group and blend space stay foot-aligned.
  auto push = [&](kinema::OwnedClip c) {
    state->clips.push_back(std::make_unique<kinema::OwnedClip>(std::move(c)));
    return static_cast<int>(state->clips.size()) - 1;
  };
  state->idle_clip = push(BakeGait(sk, 61, 30.0f, GaitAmp{0.03f, 0.04f, 0.05f, 0.05f, 0.0f},
                                   /*stride=*/0.0f, /*footfalls=*/false));
  state->walk_clip = push(BakeGait(sk, 31, 30.0f, GaitAmp{0.50f, 0.70f, 0.35f, 0.06f, 0.05f},
                                   /*stride=*/1.6f, /*footfalls=*/true));
  state->run_clip = push(BakeGait(sk, 19, 30.0f, GaitAmp{0.85f, 1.10f, 0.60f, 0.07f, 0.22f},
                                  /*stride=*/2.7f, /*footfalls=*/true));
  state->walk_speed = 1.6f;
  state->run_speed = 4.5f;

  // 1D walk<->run blend space keyed on the "speed" parameter (idle is a separate
  // state, so the space is exactly the two synced gaits).
  state->locomotion_space = std::make_unique<kinema::BlendSpace>(kinema::BlendSpace::Dim::k1D);
  state->locomotion_space->Add(state->clip(state->walk_clip), state->walk_speed);
  state->locomotion_space->Add(state->clip(state->run_clip), state->run_speed);
  state->locomotion_space->Finalize();

  // Two-state machine: idle <-> locomotion, transitions driven by "speed".
  kinema::StateMachineBuilder smb(sk.count());
  state->idle_state = smb.AddClipState(state->clip(state->idle_clip), /*loop=*/true);

  // The locomotion state is a program fragment holding one blend-space op whose
  // normalized phase (time_param) the RigPlayer drives from a foot-sync group,
  // so cadence tracks speed. clip/root_source = walk supplies events + root.
  kinema::PoseOp loco{};
  loco.kind = kinema::PoseOp::Kind::kBlendSpace;
  loco.dst = 0;
  loco.a = 1;  // scratch register
  loco.space = state->locomotion_space.get();
  loco.coord_param = static_cast<kinema::i16>(state->speed_param);
  loco.time_param = static_cast<kinema::i16>(state->phase_param);
  const f32 walk_dur = state->clip(state->walk_clip)->duration();
  state->loco_state =
      smb.AddProgramState(&loco, 1, /*reg_count=*/2, /*clock_op=*/-1, /*loop_duration=*/walk_dur,
                          /*loop=*/true, /*speed=*/1.0f, /*clip=*/state->clip(state->walk_clip),
                          /*root_source=*/state->clip(state->walk_clip));

  kinema::TransitionDesc start;
  start.duration = 0.22f;  // inertialized ramp into locomotion
  kinema::ConditionAtom moving = kinema::ConditionAtom::Greater(state->speed_param, 0.5f);
  smb.AddTransition(state->idle_state, state->loco_state, start, &moving, 1);

  kinema::TransitionDesc stop;
  stop.duration = 0.28f;
  kinema::ConditionAtom halting = kinema::ConditionAtom::Less(state->speed_param, 0.35f);
  smb.AddTransition(state->loco_state, state->idle_state, stop, &halting, 1);

  state->machine = smb.Build();
  return AnimGraph(std::move(state));
}

u32 AnimGraph::bone_count() const { return state_ ? state_->skeleton.count() : 0; }

int AnimGraph::ParamIndex(std::string_view name) const {
  if (!state_) return -1;
  for (size_t i = 0; i < state_->param_names.size(); ++i) {
    if (state_->param_names[i] == name) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace rx::anim
