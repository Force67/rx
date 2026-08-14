#include "anim/locomotion.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace rx::anim {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 2.0f * kPi;
constexpr f32 kStride = 1.5f; // meters per gait cycle, sets step frequency

f32 Clamp01(f32 v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

f32 Mix(f32 a, f32 b, f32 weight) { return a + (b - a) * weight; }

i32 FindFirst(const asset::Skeleton &skeleton,
              std::initializer_list<std::string_view> names) {
  for (std::string_view name : names) {
    const i32 bone = skeleton.Find(name);
    if (bone >= 0)
      return bone;
  }
  return -1;
}

// Driven joints resolve through a short list of explicit rig conventions.
// Missing portions are skipped, allowing a partial biped to use the same API.

// Layer a local-space rotation delta about `axis` onto a bone's bind pose.
void LayerRotation(SkeletonPose *pose, i32 bone, const Vec3 &axis,
                   f32 radians) {
  if (bone < 0 || bone >= static_cast<i32>(pose->rotation.size()))
    return;
  pose->rotation[bone] =
      Normalize(pose->rotation[bone] * QuatFromAxisAngle(axis, radians));
}

} // namespace

WalkStyle MakeWalkStylePreset(WalkStyleKind kind) {
  WalkStyle style;
  switch (kind) {
  case WalkStyleKind::kNeutral:
    break;
  case WalkStyleKind::kHipSway:
    style.cadence_scale = 0.92f;
    style.stride_scale = 0.92f;
    style.knee_lift_scale = 0.82f;
    style.arm_swing_scale = 0.78f;
    style.elbow_bend = 0.08f;
    style.hip_roll = 0.145f;
    style.hip_yaw = 0.075f;
    style.hip_shift = 0.042f;
    style.vertical_bob = 0.012f;
    style.torso_counter_roll = 0.085f;
    style.torso_counter_yaw = 0.045f;
    break;
  case WalkStyleKind::kMarch:
    style.cadence_scale = 1.08f;
    style.stride_scale = 1.42f;
    style.knee_lift_scale = 1.62f;
    style.arm_swing_scale = 1.48f;
    style.elbow_bend = 0.25f;
    style.hip_roll = 0.018f;
    style.hip_yaw = 0.018f;
    style.hip_shift = 0.004f;
    style.vertical_bob = 0.026f;
    style.torso_counter_roll = 0.012f;
    style.torso_counter_yaw = 0.018f;
    break;
  }
  return style;
}

WalkStyle BlendWalkStyles(const WalkStyle &a, const WalkStyle &b, f32 weight) {
  const f32 t = Clamp01(weight);
  WalkStyle out;
  out.cadence_scale = Mix(a.cadence_scale, b.cadence_scale, t);
  out.stride_scale = Mix(a.stride_scale, b.stride_scale, t);
  out.knee_lift_scale = Mix(a.knee_lift_scale, b.knee_lift_scale, t);
  out.arm_swing_scale = Mix(a.arm_swing_scale, b.arm_swing_scale, t);
  out.elbow_bend = Mix(a.elbow_bend, b.elbow_bend, t);
  out.hip_roll = Mix(a.hip_roll, b.hip_roll, t);
  out.hip_yaw = Mix(a.hip_yaw, b.hip_yaw, t);
  out.hip_shift = Mix(a.hip_shift, b.hip_shift, t);
  out.vertical_bob = Mix(a.vertical_bob, b.vertical_bob, t);
  out.torso_counter_roll = Mix(a.torso_counter_roll, b.torso_counter_roll, t);
  out.torso_counter_yaw = Mix(a.torso_counter_yaw, b.torso_counter_yaw, t);
  return out;
}

const char *WalkStyleName(WalkStyleKind kind) {
  switch (kind) {
  case WalkStyleKind::kNeutral:
    return "Neutral";
  case WalkStyleKind::kHipSway:
    return "Hip Sway";
  case WalkStyleKind::kMarch:
    return "March";
  }
  return "Unknown";
}

f32 AdvancePhase(f32 phase, f32 speed, f32 dt) {
  return AdvancePhase(phase, speed, dt, WalkStyle{});
}

f32 AdvancePhase(f32 phase, f32 speed, f32 dt, const WalkStyle &style) {
  f32 cycles_per_second =
      speed > 0.01f ? speed / kStride : 0.5f; // idle ticks slowly
  phase += cycles_per_second * std::max(style.cadence_scale, 0.0f) * dt;
  phase -= std::floor(phase);
  return phase;
}

void Locomotion::Apply(const asset::Skeleton &skeleton, f32 speed,
                       SkeletonPose *pose) const {
  if (!pose)
    return;
  pose->ResetToBind(skeleton);

  // Gait amplitude grows from a tiny idle sway through walk into a big run.
  f32 walk = Clamp01(speed / 1.5f);
  f32 run = Clamp01((speed - 1.5f) / 3.5f);
  f32 theta = phase * kTwoPi;

  if (!is_biped) {
    // Quadrupeds/creatures: a slow breathing bob on the spine, no stepping.
    f32 breathe = std::sin(theta) * 0.03f;
    const i32 spine =
        FindFirst(skeleton, {"NPC Spine [Spn0]", "spine", "spine-1", "chest"});
    LayerRotation(pose, spine, {1, 0, 0}, breathe);
    return;
  }

  const i32 hip = FindFirst(skeleton, {"NPC Pelvis [Pelv]", "hip", "pelvis"});
  const i32 spine =
      FindFirst(skeleton, {"NPC Spine1 [Spn1]", "chest", "spine-1", "spine"});
  const i32 left_thigh =
      FindFirst(skeleton, {"NPC L Thigh [LThg]", "thigh.bend.L", "thigh.L"});
  const i32 right_thigh =
      FindFirst(skeleton, {"NPC R Thigh [RThg]", "thigh.bend.R", "thigh.R"});
  const i32 left_shin =
      FindFirst(skeleton, {"NPC L Calf [LClf]", "shin.bend.L", "shin.L"});
  const i32 right_shin =
      FindFirst(skeleton, {"NPC R Calf [RClf]", "shin.bend.R", "shin.R"});
  const i32 left_arm = FindFirst(
      skeleton, {"NPC L UpperArm [LUar]", "upper_arm.bend.L", "upper_arm.L"});
  const i32 right_arm = FindFirst(
      skeleton, {"NPC R UpperArm [RUar]", "upper_arm.bend.R", "upper_arm.R"});
  const i32 left_forearm = FindFirst(
      skeleton, {"NPC L Forearm [LLar]", "forearm.bend.L", "forearm.L"});
  const i32 right_forearm = FindFirst(
      skeleton, {"NPC R Forearm [RLar]", "forearm.bend.R", "forearm.R"});

  f32 leg = std::sin(theta); // left leg leads, right is opposite phase
  f32 thigh_amp = (0.06f + 0.45f * walk + 0.35f * run) * style.stride_scale;
  f32 knee_amp = (0.10f + 0.7f * walk + 0.6f * run) * style.knee_lift_scale;
  f32 arm_amp = (0.05f + 0.35f * walk + 0.45f * run) * style.arm_swing_scale;

  const Vec3 x{1, 0, 0};
  f32 ts = thigh_axis_sign;
  // Hips swing fore/aft in antiphase; knees bend most as the leg passes under.
  LayerRotation(pose, left_thigh, x, ts * thigh_amp * leg);
  LayerRotation(pose, right_thigh, x, -ts * thigh_amp * leg);
  LayerRotation(pose, left_shin, x,
                -knee_amp * Clamp01(-std::sin(theta - 0.6f)));
  LayerRotation(pose, right_shin, x,
                -knee_amp * Clamp01(-std::sin(theta + kPi - 0.6f)));

  // Arms counter-swing the legs.
  f32 as = arm_axis_sign;
  LayerRotation(pose, left_arm, x, -as * arm_amp * leg);
  LayerRotation(pose, right_arm, x, as * arm_amp * leg);
  const f32 elbow = style.elbow_bend * walk;
  LayerRotation(pose, left_forearm, x, -elbow);
  LayerRotation(pose, right_forearm, x, -elbow);

  // Style-specific pelvis motion is countered by the torso so the head remains
  // readable. Translation is scaled from the rig's own leg proportions.
  f32 leg_length = 1.0f;
  if (left_shin >= 0)
    leg_length =
        std::max(Length(skeleton.bones[left_shin].bind_translation), 1.0e-4f);
  const f32 sway = std::cos(theta) * walk;
  const f32 twist = std::sin(theta) * walk;
  LayerRotation(pose, hip, {0, 0, 1}, style.hip_roll * sway);
  LayerRotation(pose, hip, {0, 1, 0}, style.hip_yaw * twist);
  if (hip >= 0 && hip < static_cast<i32>(pose->translation.size())) {
    pose->translation[hip].x += style.hip_shift * leg_length * sway;
    pose->translation[hip].y += style.vertical_bob * leg_length * walk *
                                (0.5f - 0.5f * std::cos(theta * 2.0f));
  }
  LayerRotation(pose, spine, {0, 0, 1}, -style.torso_counter_roll * sway);
  LayerRotation(pose, spine, {0, 1, 0}, -style.torso_counter_yaw * twist);
}

} // namespace rx::anim
