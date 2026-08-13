#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "anim/locomotion.h"
#include "asset/asset_id.h"

namespace {

using namespace rx;
using namespace rx::anim;

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "walk_style_test: FAIL: %s\n", message);
  ++failures;
}

f32 RotationDelta(const Quat &a, const Quat &b) {
  const f32 dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 1.0f - std::min(dot, 1.0f);
}

void AddBone(asset::Skeleton *skeleton, const char *name, i32 parent,
             Vec3 translation = {}) {
  asset::Bone bone;
  bone.name = name;
  bone.parent = parent;
  bone.bind_translation = translation;
  skeleton->bones.push_back(bone);
}

asset::Skeleton MakeGenesisStyleSkeleton() {
  asset::Skeleton skeleton;
  skeleton.id = asset::MakeAssetId("walk-style-test-rig");
  AddBone(&skeleton, "hip", -1);
  AddBone(&skeleton, "spine-1", 0, {0, 0.2f, 0});
  AddBone(&skeleton, "thigh.bend.L", 0, {-0.1f, -0.1f, 0});
  AddBone(&skeleton, "shin.bend.L", 2, {0, -0.45f, 0});
  AddBone(&skeleton, "thigh.bend.R", 0, {0.1f, -0.1f, 0});
  AddBone(&skeleton, "shin.bend.R", 4, {0, -0.45f, 0});
  AddBone(&skeleton, "upper_arm.bend.L", 1, {-0.2f, 0.2f, 0});
  AddBone(&skeleton, "forearm.bend.L", 6, {-0.3f, 0, 0});
  AddBone(&skeleton, "upper_arm.bend.R", 1, {0.2f, 0.2f, 0});
  AddBone(&skeleton, "forearm.bend.R", 8, {0.3f, 0, 0});
  return skeleton;
}

void TestPresetsAndBlend() {
  const WalkStyle sway = MakeWalkStylePreset(WalkStyleKind::kHipSway);
  const WalkStyle march = MakeWalkStylePreset(WalkStyleKind::kMarch);
  Check(sway.hip_roll > march.hip_roll * 4.0f,
        "Hip Sway emphasizes pelvis roll");
  Check(sway.hip_shift > march.hip_shift * 4.0f,
        "Hip Sway emphasizes lateral pelvis travel");
  Check(march.stride_scale > sway.stride_scale, "March has a longer stride");
  Check(march.knee_lift_scale > sway.knee_lift_scale,
        "March has higher knee lift");
  Check(march.arm_swing_scale > sway.arm_swing_scale,
        "March has stronger arm swing");

  const WalkStyle midpoint = BlendWalkStyles(sway, march, 0.5f);
  Check(midpoint.hip_roll < sway.hip_roll && midpoint.hip_roll > march.hip_roll,
        "style blending interpolates pelvis motion");
  Check(midpoint.knee_lift_scale > sway.knee_lift_scale &&
            midpoint.knee_lift_scale < march.knee_lift_scale,
        "style blending interpolates knee lift");

  const f32 sway_phase = AdvancePhase(0, 1.35f, 0.25f, sway);
  const f32 march_phase = AdvancePhase(0, 1.35f, 0.25f, march);
  Check(march_phase > sway_phase, "style cadence affects phase advance");
  Check(std::string(WalkStyleName(WalkStyleKind::kHipSway)) == "Hip Sway" &&
            std::string(WalkStyleName(WalkStyleKind::kMarch)) == "March",
        "walk styles have stable editor-facing names");
}

void TestGenesisAliases() {
  const asset::Skeleton skeleton = MakeGenesisStyleSkeleton();
  SkeletonPose sway_pose;
  SkeletonPose march_pose;

  Locomotion sway;
  sway.phase = 0.125f;
  sway.style = MakeWalkStylePreset(WalkStyleKind::kHipSway);
  sway.Apply(skeleton, 1.35f, &sway_pose);

  Locomotion march;
  march.phase = sway.phase;
  march.style = MakeWalkStylePreset(WalkStyleKind::kMarch);
  march.Apply(skeleton, 1.35f, &march_pose);

  const i32 hip = skeleton.Find("hip");
  const i32 thigh = skeleton.Find("thigh.bend.L");
  const i32 arm = skeleton.Find("upper_arm.bend.L");
  Check(RotationDelta(skeleton.bones[hip].bind_rotation,
                      sway_pose.rotation[hip]) >
            RotationDelta(skeleton.bones[hip].bind_rotation,
                          march_pose.rotation[hip]) *
                2.0f,
        "Genesis aliases receive stronger Hip Sway pelvis rotation");
  Check(std::fabs(sway_pose.translation[hip].x) >
            std::fabs(march_pose.translation[hip].x) * 4.0f,
        "Genesis hip receives stronger Hip Sway lateral travel");
  Check(RotationDelta(skeleton.bones[thigh].bind_rotation,
                      march_pose.rotation[thigh]) >
            RotationDelta(skeleton.bones[thigh].bind_rotation,
                          sway_pose.rotation[thigh]),
        "Genesis thigh receives the longer March stride");
  Check(RotationDelta(skeleton.bones[arm].bind_rotation,
                      march_pose.rotation[arm]) >
            RotationDelta(skeleton.bones[arm].bind_rotation,
                          sway_pose.rotation[arm]),
        "Genesis arm receives the stronger March counter-swing");

  base::Vector<Mat4> matrices;
  ComputeModelMatrices(skeleton, march_pose, &matrices);
  Check(matrices.size() == skeleton.bones.size(),
        "styled Genesis pose produces a complete model palette");
  for (const Mat4 &matrix : matrices)
    for (f32 value : matrix.m)
      Check(std::isfinite(value), "styled pose matrices stay finite");
}

} // namespace

int main() {
  TestPresetsAndBlend();
  TestGenesisAliases();
  if (failures == 0) {
    std::puts("walk_style_test: PASS");
    return 0;
  }
  std::fprintf(stderr, "walk_style_test: %d failure(s)\n", failures);
  return 1;
}
