#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "anim/body_dynamics.h"
#include "anim/locomotion.h"
#include "anim/pose.h"
#include "asset/asset_id.h"
#include "asset/blend_import.h"
#include "asset/gltf_loader.h"

namespace {

using namespace rx;

bool Check(bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "blend_jiggle_test: FAIL: %s\n", message);
  return condition;
}

f32 MatrixDifference(const Mat4 &a, const Mat4 &b) {
  f32 difference = 0;
  for (u32 i = 0; i < 16; ++i)
    difference = std::max(difference, std::fabs(a.m[i] - b.m[i]));
  return difference;
}

f32 RotationDifference(const Quat &a, const Quat &b) {
  const f32 dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return 1.0f - std::min(dot, 1.0f);
}

int TestBlend(const std::string &blend_path, const std::string &script) {
  asset::BlendImportOptions options;
  options.converter_script = script;
  asset::BlendImportResult converted;
  std::string error;
  const bool converted_ok =
      asset::ConvertBlendScene(blend_path, options, &converted, &error);
  if (!Check(converted_ok,
             error.empty() ? "Blender conversion succeeds" : error.c_str()))
    return 1;

  asset::GltfScene scene;
  if (!Check(asset::LoadGltfScene(converted.glb_path, &scene),
             "converted GLB loads"))
    return 1;
  if (!Check(!scene.meshes.empty() && !scene.instances.empty(),
             "converted scene contains meshes and instances"))
    return 1;

  i32 skeleton_index = -1;
  for (u32 i = 0; i < scene.skeletons.size(); ++i) {
    const asset::Skeleton &candidate = scene.skeletons[i];
    if (candidate.Find("chest.twk") >= 0 && candidate.Find("pectoral.L") >= 0 &&
        candidate.Find("pectoral.R") >= 0) {
      skeleton_index = static_cast<i32>(i);
      break;
    }
  }
  if (!Check(skeleton_index >= 0,
             "rig contains the driver and both chest deformation bones"))
    return 1;
  const asset::Skeleton &skeleton = scene.skeletons[skeleton_index];
  for (u32 i = 0; i < skeleton.bones.size(); ++i) {
    if (!Check(skeleton.bones[i].parent < static_cast<i32>(i),
               "skeleton is topologically ordered"))
      return 1;
  }

  const i32 hip = skeleton.Find("hip");
  const i32 left_thigh = skeleton.Find("thigh.bend.L");
  if (!Check(hip >= 0 && left_thigh >= 0,
             "Lara rig exposes Genesis walk-style bones"))
    return 1;
  anim::SkeletonPose hip_sway_pose;
  anim::SkeletonPose march_pose;
  anim::Locomotion hip_sway;
  hip_sway.phase = 0.125f;
  hip_sway.style = anim::MakeWalkStylePreset(anim::WalkStyleKind::kHipSway);
  hip_sway.Apply(skeleton, 1.35f, &hip_sway_pose);
  anim::Locomotion march;
  march.phase = hip_sway.phase;
  march.style = anim::MakeWalkStylePreset(anim::WalkStyleKind::kMarch);
  march.Apply(skeleton, 1.35f, &march_pose);
  if (!Check(RotationDifference(skeleton.bones[hip].bind_rotation,
                                hip_sway_pose.rotation[hip]) >
                 RotationDifference(skeleton.bones[hip].bind_rotation,
                                    march_pose.rotation[hip]) *
                     2.0f,
             "Hip Sway produces more hip rotation on Lara"))
    return 1;
  if (!Check(RotationDifference(skeleton.bones[left_thigh].bind_rotation,
                                march_pose.rotation[left_thigh]) >
                 RotationDifference(skeleton.bones[left_thigh].bind_rotation,
                                    hip_sway_pose.rotation[left_thigh]),
             "March produces a longer stride on Lara"))
    return 1;

  const asset::Mesh *body_mesh = nullptr;
  for (const asset::Mesh &mesh : scene.meshes) {
    if (mesh.FindMorphTarget(asset::MakeAssetId("pPBMlBreastsFlatten").hash) >=
            0 &&
        mesh.FindMorphTarget(
            asset::MakeAssetId("pPBMrBreastsHangForward").hash) >= 0) {
      body_mesh = &mesh;
      break;
    }
  }
  if (!Check(body_mesh != nullptr,
             "body mesh retains authored chest deformation morphs"))
    return 1;
  if (!Check(body_mesh->skinned && !body_mesh->lods.empty() &&
                 body_mesh->lods[0].skinning.size() ==
                     body_mesh->lods[0].vertices.size(),
             "body mesh retains a complete skinning stream"))
    return 1;
  if (!Check(body_mesh->skin.bones.size() <= 256,
             "body skin fits the engine's byte joint indices"))
    return 1;
  for (const asset::SkinnedVertexExtra &vertex : body_mesh->lods[0].skinning) {
    for (u32 lane = 0; lane < 4; ++lane) {
      if (vertex.bone_weights[lane] == 0)
        continue;
      if (!Check(vertex.bone_indices[lane] < body_mesh->skin.bones.size(),
                 "weighted joint index is inside the skin palette"))
        return 1;
    }
  }

  anim::BodyDynamics dynamics;
  auto add_chest = [&](const char *side, const char *bone, const char *flatten,
                       const char *hang) {
    anim::BodyRegionConfig region = anim::MakeBodyRegionPreset(
        anim::BodyRegionKind::kChest, side, "chest.twk", bone);
    region.deformation_axis = {0, -1, 0};
    region.morphs.push_back(
        {flatten, anim::BodyDeformationSignal::kImpact, 0.55f});
    region.morphs.push_back(
        {hang, anim::BodyDeformationSignal::kStretch, 0.35f});
    dynamics.AddRegion(region);
  };
  add_chest("chest.left", "pectoral.L", "pPBMlBreastsFlatten",
            "pPBMlBreastsHangForward");
  add_chest("chest.right", "pectoral.R", "pPBMrBreastsFlatten",
            "pPBMrBreastsHangForward");

  anim::SkeletonPose pose;
  pose.ResetToBind(skeleton);
  base::Vector<anim::BodyMorphWeight> morphs;
  anim::BodyDynamicsFrame frame;
  dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose, &morphs);

  base::Vector<Mat4> bind_model;
  anim::ComputeModelMatrices(skeleton, pose, &bind_model);
  const base::Vector<i32> remap =
      anim::BuildBoneRemap(skeleton, body_mesh->skin);
  base::Vector<Mat4> bind_palette;
  anim::BuildSkinPalette(bind_model, body_mesh->skin, remap, &bind_palette);

  frame.linear_impulse = {0, -1.25f, 0.35f};
  for (u32 step = 0; step < 12; ++step) {
    pose.ResetToBind(skeleton);
    dynamics.Update(skeleton, frame, 1.0f / 60.0f, &pose, &morphs);
    frame.linear_impulse = {};
  }
  base::Vector<Mat4> moved_model;
  base::Vector<Mat4> moved_palette;
  anim::ComputeModelMatrices(skeleton, pose, &moved_model);
  anim::BuildSkinPalette(moved_model, body_mesh->skin, remap, &moved_palette);

  const i32 left_palette = static_cast<i32>(
      std::find(body_mesh->skin.bones.begin(), body_mesh->skin.bones.end(),
                std::string("pectoral.L")) -
      body_mesh->skin.bones.begin());
  if (!Check(left_palette >= 0 &&
                 left_palette < static_cast<i32>(moved_palette.size()),
             "body skin references the left chest deformation bone"))
    return 1;
  if (!Check(MatrixDifference(bind_palette[left_palette], Mat4::Identity()) <
                 0.002f,
             "bind pose reconstructs an identity skin palette"))
    return 1;
  const f32 palette_motion =
      MatrixDifference(bind_palette[left_palette], moved_palette[left_palette]);
  if (!Check(palette_motion > 1e-5f,
             "landing impulse moves the chest skin palette"))
    return 1;

  base::Vector<f32> dense_morphs;
  anim::ApplyBodyMorphWeights(*body_mesh, morphs, &dense_morphs);
  const i32 flatten_index = body_mesh->FindMorphTarget(
      asset::MakeAssetId("pPBMlBreastsFlatten").hash);
  if (!Check(flatten_index >= 0 && dense_morphs[flatten_index] > 0,
             "landing impulse activates Lara's authored flatten morph"))
    return 1;

  std::printf(
      "blend_jiggle_test: PASS: %llu meshes, %llu instances, %llu bones, "
      "%llu body vertices, palette delta %.6f, flatten weight %.4f, "
      "walk styles Hip Sway/March%s\n",
      static_cast<unsigned long long>(scene.meshes.size()),
      static_cast<unsigned long long>(scene.instances.size()),
      static_cast<unsigned long long>(skeleton.bones.size()),
      static_cast<unsigned long long>(body_mesh->lods[0].vertices.size()),
      palette_motion, dense_morphs[flatten_index],
      converted.reused_cache ? ", cached conversion" : "");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 1) {
    const std::string script = argc > 2 ? argv[2] : RX_BLEND_CONVERTER_SCRIPT;
    return TestBlend(argv[1], script);
  }

  // Keep the default CTest path hermetic: Blender and a user asset are not
  // required to verify that the public API rejects invalid sources cleanly.
  asset::BlendImportOptions options;
  options.converter_script = RX_BLEND_CONVERTER_SCRIPT;
  asset::BlendImportResult result;
  std::string error;
  if (!Check(
          !asset::ConvertBlendScene("missing.blend", options, &result, &error),
          "missing .blend is rejected"))
    return 1;
  if (!Check(!error.empty(), "missing .blend reports a useful error"))
    return 1;
  std::puts("blend_jiggle_test: PASS: converter API validation");
  return 0;
}
