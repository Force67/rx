#ifndef RX_ANIM_LOCOMOTION_H_
#define RX_ANIM_LOCOMOTION_H_

#include "anim/pose.h"
#include "asset/skeleton.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::anim {

// Walk profiles describe motion, not character identity. A game may select a
// preset from character metadata, player customization, mood, equipment, or
// gameplay state and may further tune every value per character.
enum class WalkStyleKind : u8 {
  kNeutral,
  kHipSway,
  kMarch,
};

struct WalkStyle {
  f32 cadence_scale = 1.0f;
  f32 stride_scale = 1.0f;
  f32 knee_lift_scale = 1.0f;
  f32 arm_swing_scale = 1.0f;
  f32 elbow_bend = 0.06f;
  f32 hip_roll = 0.04f;
  f32 hip_yaw = 0.025f;
  f32 hip_shift = 0.012f;   // fraction of estimated leg length
  f32 vertical_bob = 0.01f; // fraction of estimated leg length
  f32 torso_counter_roll = 0.03f;
  f32 torso_counter_yaw = 0.02f;
};

RX_ANIM_EXPORT WalkStyle MakeWalkStylePreset(WalkStyleKind kind);
RX_ANIM_EXPORT WalkStyle BlendWalkStyles(const WalkStyle &a, const WalkStyle &b,
                                         f32 weight);
RX_ANIM_EXPORT const char *WalkStyleName(WalkStyleKind kind);

// Procedural biped locomotion: synthesizes an idle/walk/run gait directly on a
// skeleton. It recognizes both the legacy "NPC ..." convention and common
// Genesis/Blender deform-bone names. No clips or rig-specific runtime are
// required. Foot IK can refine the result against the ground afterwards.
//
// `speed` is planar m/s and blends gait amplitude/frequency; `phase` is the
// 0..1 gait cycle position (advance it with AdvancePhase each frame). The pose
// is reset to bind internally, then the driven joints are rotated.
struct RX_ANIM_EXPORT Locomotion {
  f32 phase = 0;
  WalkStyle style;
  // Tuning, exposed so the same code drives a hand-built test rig and a real
  // game skeleton (whose bone axes differ); signs flip per skeleton.
  f32 thigh_axis_sign = 1.0f;
  f32 arm_axis_sign = 1.0f;
  bool is_biped = true; // non-bipeds get a breathing idle only

  void Apply(const asset::Skeleton &skeleton, f32 speed,
             SkeletonPose *pose) const;
};

// Advance and wrap the 0..1 gait phase for one frame at the given planar speed.
RX_ANIM_EXPORT f32 AdvancePhase(f32 phase, f32 speed, f32 dt);
RX_ANIM_EXPORT f32 AdvancePhase(f32 phase, f32 speed, f32 dt,
                                const WalkStyle &style);

} // namespace rx::anim

#endif // RX_ANIM_LOCOMOTION_H_
