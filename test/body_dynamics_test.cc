#include <algorithm>
#include <cmath>
#include <cstdio>

#include "anim/body_dynamics.h"
#include "asset/asset_id.h"

namespace {

using namespace rx;
using namespace rx::anim;

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "body_dynamics_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, f32 tolerance, const char *message) {
  if (std::fabs(actual - expected) <= tolerance)
    return;
  std::fprintf(
      stderr,
      "body_dynamics_test: FAIL: %s (got %.6f, expected %.6f +/- %.6f)\n",
      message, actual, expected, tolerance);
  ++failures;
}

asset::Skeleton MakeSkeleton() {
  asset::Skeleton skeleton;
  skeleton.id = asset::MakeAssetId("body-test-rig");
  asset::Bone root;
  root.name = "root";
  root.parent = -1;
  skeleton.bones.push_back(root);
  asset::Bone driver;
  driver.name = "torso";
  driver.parent = 0;
  driver.bind_translation = {0, 1, 0};
  skeleton.bones.push_back(driver);
  asset::Bone soft;
  soft.name = "soft";
  soft.parent = 1;
  soft.bind_translation = {0, 0.2f, 0};
  skeleton.bones.push_back(soft);
  return skeleton;
}

BodyRegionConfig TestRegion() {
  BodyRegionConfig region =
      MakeBodyRegionPreset(BodyRegionKind::kChest, "test", "torso", "soft");
  region.frequency_hz = 2.0f;
  region.damping_ratio = 0.6f;
  region.translation_mobility = {1, 1, 1};
  region.max_translation = {1, 1, 1};
  region.rotation_mobility = {1, 1, 1};
  region.max_rotation = {1, 1, 1};
  return region;
}

BodyRegionSample Simulate(f32 step, f32 seconds,
                          const BodyDynamicsFrame &frame) {
  const asset::Skeleton skeleton = MakeSkeleton();
  SkeletonPose pose;
  pose.ResetToBind(skeleton);
  BodyDynamics dynamics;
  dynamics.AddRegion(TestRegion());
  dynamics.Update(skeleton, frame, step, &pose); // initialize
  const u32 count = static_cast<u32>(std::ceil(seconds / step));
  for (u32 i = 0; i < count; ++i) {
    pose.ResetToBind(skeleton);
    dynamics.Update(skeleton, frame, std::min(step, seconds - i * step), &pose);
  }
  return dynamics.sample(0);
}

void TestGravityAndInertia() {
  BodyDynamicsFrame gravity;
  const BodyRegionSample sag = Simulate(1.0f / 120.0f, 2.0f, gravity);
  const f32 expected =
      -9.81f / std::pow(2.0f * 3.14159265358979323846f * 2.0f, 2.0f);
  Near(sag.translation.y, expected, 0.0005f,
       "settled sag follows the spring's physical equilibrium");

  BodyDynamicsFrame accelerating;
  accelerating.gravity = {};
  accelerating.linear_acceleration = {4, 0, 0};
  const BodyRegionSample lag = Simulate(1.0f / 120.0f, 2.0f, accelerating);
  Check(lag.translation.x < -0.02f,
        "soft tissue lags opposite character acceleration");

  BodyDynamicsFrame turning;
  turning.gravity = {};
  turning.angular_acceleration = {5, 0, 0};
  const BodyRegionSample rotational_lag =
      Simulate(1.0f / 120.0f, 1.0f, turning);
  Check(rotational_lag.rotation.x < 0,
        "soft region rotation lags opposite character angular acceleration");
}

void TestFrameRateIndependence() {
  BodyDynamicsFrame frame;
  frame.linear_acceleration = {2.5f, 1.0f, -3.0f};
  const BodyRegionSample fine = Simulate(1.0f / 240.0f, 0.73f, frame);
  const BodyRegionSample coarse = Simulate(1.0f / 30.0f, 0.73f, frame);
  Near(coarse.translation.x, fine.translation.x, 0.00003f,
       "analytical spring is frame-rate independent on x");
  Near(coarse.translation.y, fine.translation.y, 0.00003f,
       "analytical spring is frame-rate independent on y");
  Near(coarse.translation.z, fine.translation.z, 0.00003f,
       "analytical spring is frame-rate independent on z");
}

void TestLimitsAndTeleport() {
  const asset::Skeleton skeleton = MakeSkeleton();
  SkeletonPose pose;
  pose.ResetToBind(skeleton);
  BodyRegionConfig region = TestRegion();
  region.max_translation = {0.01f, 0.02f, 0.03f};
  BodyDynamics dynamics;
  dynamics.AddRegion(region);

  BodyDynamicsFrame frame;
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose);
  frame.linear_impulse = {100, -100, 100};
  pose.ResetToBind(skeleton);
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose);
  BodyRegionSample sample = dynamics.sample(0);
  Check(std::fabs(sample.translation.x) <= 0.010001f &&
            std::fabs(sample.translation.y) <= 0.020001f &&
            std::fabs(sample.translation.z) <= 0.030001f,
        "per-axis anatomical limits hold under extreme impulses");

  frame = {};
  frame.teleport = true;
  pose.ResetToBind(skeleton);
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose);
  sample = dynamics.sample(0);
  Near(Length(sample.translation), 0, 1e-7f,
       "teleport resets displacement without a kick");
  Near(Length(sample.velocity), 0, 1e-7f, "teleport resets carried velocity");
  Check(sample.active, "teleport keeps the configured region resolved");
  frame.teleport = false;
  frame.linear_impulse = {0, -1, 0};
  pose.ResetToBind(skeleton);
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose);
  Check(std::fabs(dynamics.sample(0).translation.y) > 0,
        "region resumes simulation on the frame after a teleport");
}

void TestPoseAndMorphOutput() {
  const asset::Skeleton skeleton = MakeSkeleton();
  SkeletonPose pose;
  pose.ResetToBind(skeleton);
  BodyRegionConfig region = TestRegion();
  region.gravity_scale = 0;
  region.morphs.push_back(
      {"softCompression", BodyDeformationSignal::kCompression, 2.0f, 0, 0, 1});
  region.morphs.push_back(
      {"softImpact", BodyDeformationSignal::kImpact, 1.0f, 0, 0, 1});
  region.morphs.push_back(
      {"softBaseline", BodyDeformationSignal::kStretch, 0, 0.15f, 0, 1});
  BodyDynamics dynamics;
  dynamics.AddRegion(region);
  BodyDynamicsFrame frame;
  base::Vector<BodyMorphWeight> weights;
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose, &weights);
  Check(weights.size() == 1 && std::fabs(weights[0].weight - 0.15f) < 1e-6f,
        "morph bias is emitted on the rest/initialization frame");
  frame.linear_impulse = {0, -1, 0};
  pose.ResetToBind(skeleton);
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose, &weights);
  Check(Length(pose.translation[2] - skeleton.bones[2].bind_translation) > 0,
        "secondary translation is layered onto the driven bone");
  Check(weights.size() == 3,
        "configured deformation and impact morphs are emitted");

  asset::Mesh mesh;
  asset::MorphTarget compression;
  compression.name = "softCompression";
  compression.name_hash = asset::MakeAssetId(compression.name).hash;
  mesh.morph_targets.push_back(std::move(compression));
  asset::MorphTarget unrelated;
  unrelated.name = "expression";
  unrelated.name_hash = asset::MakeAssetId(unrelated.name).hash;
  mesh.morph_targets.push_back(std::move(unrelated));
  base::Vector<f32> dense;
  dense.push_back(0.1f);
  dense.push_back(0.7f);
  ApplyBodyMorphWeights(mesh, weights, &dense);
  Check(dense[0] >= 0.1f, "body morph adds to an existing animation weight");
  Near(dense[1], 0.7f, 1e-7f, "unbound expression morph is preserved");
}

void TestPresetsAndMissingBones() {
  const BodyRegionKind kinds[] = {
      BodyRegionKind::kChest,    BodyRegionKind::kAbdomen,
      BodyRegionKind::kGlutes,   BodyRegionKind::kThigh,
      BodyRegionKind::kUpperArm, BodyRegionKind::kCalf,
      BodyRegionKind::kCustom,
  };
  for (BodyRegionKind kind : kinds) {
    const BodyRegionConfig region =
        MakeBodyRegionPreset(kind, "region", "driver", "soft");
    Check(region.frequency_hz > 0 && region.damping_ratio > 0,
          "every standard body-region preset has stable physical tuning");
  }

  const asset::Skeleton skeleton = MakeSkeleton();
  SkeletonPose pose;
  pose.ResetToBind(skeleton);
  BodyRegionConfig missing = TestRegion();
  missing.driven_bone = "not-in-this-rig";
  BodyDynamics dynamics;
  dynamics.AddRegion(missing);
  dynamics.Update(skeleton, {}, 1.0f / 60.0f, &pose);
  Check(!dynamics.sample(0).active,
        "missing optional helper bones disable only their region");
}

void TestModelUnitScale() {
  asset::Skeleton skeleton = MakeSkeleton();
  for (asset::Bone &bone : skeleton.bones)
    bone.bind_translation = bone.bind_translation * 100.0f;
  SkeletonPose pose;
  pose.ResetToBind(skeleton);
  BodyDynamics dynamics;
  BodyRegionConfig region = TestRegion();
  region.gravity_scale = 0;
  dynamics.AddRegion(region);
  BodyDynamicsFrame frame;
  frame.model_units_per_metre = 100;
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose);
  frame.linear_impulse = {0, -1, 0};
  pose.ResetToBind(skeleton);
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose);
  const BodyRegionSample result = dynamics.sample(0);
  Check(result.active,
        "changing the source-unit scale keeps the region active");
  Check(std::fabs(result.translation.y) > 0,
        "source-unit scaled simulation responds to a physical impulse");
  Near((pose.translation[2].y - skeleton.bones[2].bind_translation.y) / 100.0f,
       result.translation.y, 1e-6f,
       "SI displacement converts back into source-native skeleton units");
}

} // namespace

int main() {
  TestGravityAndInertia();
  TestFrameRateIndependence();
  TestLimitsAndTeleport();
  TestPoseAndMorphOutput();
  TestPresetsAndMissingBones();
  TestModelUnitScale();
  if (failures == 0) {
    std::printf("body_dynamics_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "body_dynamics_test: %d checks failed\n", failures);
  return 1;
}
