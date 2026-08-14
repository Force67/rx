#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

#include "anim/body_dynamics.h"
#include "asset/asset_id.h"

namespace {

using namespace rx;
using namespace rx::anim;

asset::Skeleton MakeExampleRig() {
  asset::Skeleton skeleton;
  skeleton.id = asset::MakeAssetId("body-jiggle-example-rig");

  asset::Bone root;
  root.name = "Root";
  skeleton.bones.push_back(root);

  asset::Bone torso;
  torso.name = "Torso";
  torso.parent = 0;
  torso.bind_translation = {0, 1.0f, 0};
  skeleton.bones.push_back(torso);

  asset::Bone chest;
  chest.name = "ChestSoft";
  chest.parent = 1;
  chest.bind_translation = {0, 0.32f, 0.08f};
  skeleton.bones.push_back(chest);

  asset::Bone abdomen;
  abdomen.name = "AbdomenSoft";
  abdomen.parent = 1;
  abdomen.bind_translation = {0, 0.08f, 0.03f};
  skeleton.bones.push_back(abdomen);
  return skeleton;
}

f32 MorphWeight(const base::Vector<BodyMorphWeight> &weights,
                std::string_view name) {
  const rx::u64 hash = asset::MakeAssetId(name).hash;
  for (const BodyMorphWeight &weight : weights) {
    if (weight.target == hash)
      return weight.weight;
  }
  return 0;
}

std::string Meter(f32 displacement) {
  constexpr int kHalfWidth = 12;
  // One cell is 4 mm. Positive motion appears right of the rest marker.
  const int cell =
      std::clamp(static_cast<int>(std::round(displacement / 0.004f)),
                 -kHalfWidth, kHalfWidth);
  std::string meter(kHalfWidth * 2 + 1, ' ');
  meter[kHalfWidth] = '|';
  if (cell != 0)
    meter[kHalfWidth + cell] = 'o';
  return meter;
}

} // namespace

int main() {
  const asset::Skeleton skeleton = MakeExampleRig();
  SkeletonPose pose;
  pose.ResetToBind(skeleton);

  BodyDynamics body;
  BodyRegionConfig chest = MakeBodyRegionPreset(BodyRegionKind::kChest, "chest",
                                                "Torso", "ChestSoft");
  chest.morphs.push_back(
      {"chestCompression", BodyDeformationSignal::kCompression, 0.9f});
  chest.morphs.push_back(
      {"chestImpact", BodyDeformationSignal::kImpact, 0.65f});
  body.AddRegion(chest);

  BodyRegionConfig abdomen = MakeBodyRegionPreset(
      BodyRegionKind::kAbdomen, "abdomen", "Torso", "AbdomenSoft");
  abdomen.morphs.push_back(
      {"abdomenCompression", BodyDeformationSignal::kCompression, 0.7f});
  body.AddRegion(abdomen);

  constexpr f32 kDt = 1.0f / 60.0f;
  constexpr rx::u32 kFrames = 240;
  base::Vector<BodyMorphWeight> morphs;

  std::puts(
      "Body jiggle example (o = vertical helper-bone displacement, | = rest)");
  std::puts(" event        time  chest                     abdomen             "
            "      compress impact");

  for (rx::u32 frame_index = 0; frame_index < kFrames; ++frame_index) {
    const f32 time = frame_index * kDt;
    pose.ResetToBind(skeleton); // base animation/IK would write the pose here

    // A subtle gait bob exercises animation-derived driver acceleration.
    pose.translation[1].y += std::sin(time * 3.14159265f * 4.0f) * 0.006f;

    BodyDynamicsFrame frame;
    const char *event = "settle";
    if (time < 0.55f) {
      frame.linear_acceleration = {0, 0, -5.0f}; // sprint start
      event = "accelerate";
    } else if (time < 1.15f) {
      event = "coast";
    } else if (frame_index == 69) {
      frame.linear_impulse = {0, -1.8f, 0}; // one-frame landing impulse
      event = "LAND";
    } else if (time < 2.0f) {
      event = "rebound";
    } else if (time < 2.35f) {
      frame.angular_acceleration = {0, 8.0f, 0}; // sharp turn
      event = "turn";
    }

    body.Update(skeleton, frame, kDt, &pose, &morphs);

    if (frame_index % 4 != 0 && frame_index != 69)
      continue;
    const BodyRegionSample chest_sample = body.sample(0);
    const BodyRegionSample abdomen_sample = body.sample(1);
    const f32 compression = MorphWeight(morphs, "chestCompression");
    const f32 impact = MorphWeight(morphs, "chestImpact");
    std::printf(" %-10s %5.2f  [%s]  [%s]    %5.2f   %5.2f\n", event, time,
                Meter(chest_sample.translation.y).c_str(),
                Meter(abdomen_sample.translation.y).c_str(), compression,
                impact);
  }

  std::puts("\nIn a renderer, upload pose after BodyDynamics::Update and add "
            "the emitted");
  std::puts("body morph weights to the character's existing dense morph-weight "
            "array.");
  return 0;
}
