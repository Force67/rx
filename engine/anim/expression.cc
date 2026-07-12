#include "anim/expression.h"

#include <algorithm>
#include <cmath>

#include "asset/asset_id.h"

namespace rx::anim {
namespace {

// Critically-damped spring toward `goal` (exact integration, stable for any
// dt). `halflife` is the time the remaining offset takes to roughly halve; no
// overshoot from rest and only vanishing overshoot from a carried velocity.
void Damp(f32* x, f32* v, f32 goal, f32 halflife, f32 dt) {
  const f32 y = (2.0f * 0.69314718f) / std::max(halflife, 1e-4f);
  const f32 j0 = *x - goal;
  const f32 j1 = *v + j0 * y;
  const f32 e = std::exp(-y * dt);
  *x = goal + (j0 + j1 * dt) * e;
  *v = (*v - j1 * y * dt) * e;
}

// The transition_time scale is relative to how long the default region table
// takes to visibly settle.
constexpr f32 kDefaultTransition = 0.4f;

}  // namespace

ExpressionController::ExpressionController() {
  // Default region response: eyes and brows lead, nose/cheek follow, the
  // mouth and jaw trail by tens of milliseconds with a softer response. The
  // empty prefix is the fallback for unclassified targets (e.g. tongueOut).
  const Region defaults[] = {
      {"eye", 0.045f, 0.0f},   {"brow", 0.055f, 0.0f}, {"nose", 0.09f, 0.02f},
      {"cheek", 0.10f, 0.03f}, {"viseme", 0.12f, 0.04f}, {"mouth", 0.12f, 0.05f},
      {"jaw", 0.14f, 0.06f},   {"", 0.10f, 0.02f},
  };
  SetRegions(defaults, static_cast<u32>(std::size(defaults)));
}

void ExpressionController::SetRegions(const Region* regions, u32 count) {
  regions_.clear();
  for (u32 i = 0; i < count; ++i) regions_.push_back(regions[i]);
}

u32 ExpressionController::EnsureChannel(std::string_view name) {
  const u64 target = asset::MakeAssetId(name).hash;
  for (u32 i = 0; i < channels_.size(); ++i) {
    if (channels_[i].target == target) return i;
  }
  Channel channel;
  channel.name = std::string(name);
  channel.target = target;
  // First matching prefix wins; an empty prefix matches everything.
  for (const Region& region : regions_) {
    if (name.substr(0, region.prefix.size()) != region.prefix) continue;
    channel.base_halflife = channel.halflife = region.halflife;
    channel.base_delay = region.delay;
    break;
  }
  channels_.push_back(std::move(channel));
  return static_cast<u32>(channels_.size() - 1);
}

void ExpressionController::AddPose(std::string_view name, const PoseEntry* entries, u32 count) {
  Pose pose;
  pose.name_hash = asset::MakeAssetId(name).hash;
  for (u32 i = 0; i < count; ++i) {
    pose.channel.push_back(EnsureChannel(entries[i].target));
    pose.weight.push_back(entries[i].weight);
  }
  for (Pose& existing : poses_) {
    if (existing.name_hash != pose.name_hash) continue;
    existing = std::move(pose);
    return;
  }
  poses_.push_back(std::move(pose));
}

void ExpressionController::AddDefaultPoses() {
  AddPose("neutral", {});
  AddPose("smile", {{"mouthSmileLeft", 0.75f},
                    {"mouthSmileRight", 0.75f},
                    {"cheekSquintLeft", 0.3f},
                    {"cheekSquintRight", 0.3f},
                    {"mouthDimpleLeft", 0.3f},
                    {"mouthDimpleRight", 0.3f},
                    {"eyeSquintLeft", 0.25f},
                    {"eyeSquintRight", 0.25f}});
  AddPose("angry", {{"browDownLeft", 1.0f},
                    {"browDownRight", 1.0f},
                    {"noseSneerLeft", 0.9f},
                    {"noseSneerRight", 0.9f},
                    {"eyeSquintLeft", 0.6f},
                    {"eyeSquintRight", 0.6f},
                    {"mouthUpperUpLeft", 0.35f},
                    {"mouthUpperUpRight", 0.35f},
                    {"mouthFrownLeft", 0.3f},
                    {"mouthFrownRight", 0.3f},
                    {"mouthShrugLower", 0.3f},
                    {"jawForward", 0.25f}});
  AddPose("surprised", {{"browInnerUp", 0.9f},
                        {"eyeWideLeft", 0.8f},
                        {"eyeWideRight", 0.8f},
                        {"browOuterUpLeft", 0.7f},
                        {"browOuterUpRight", 0.7f},
                        {"jawOpen", 0.45f}});
  AddPose("eyes_closed", {{"eyeBlinkLeft", 1.0f}, {"eyeBlinkRight", 1.0f}});
  AddPose("pucker", {{"mouthPucker", 1.0f},
                     {"viseme_U", 0.5f},
                     {"mouthFunnel", 0.3f},
                     {"jawForward", 0.3f}});
  AddPose("smirk", {{"mouthSmileLeft", 0.8f},
                    {"mouthPressRight", 0.3f},
                    {"eyeSquintLeft", 0.25f},
                    {"browOuterUpRight", 0.9f},
                    {"browDownLeft", 0.2f},
                    {"cheekSquintLeft", 0.3f}});
}

bool ExpressionController::SetExpression(std::string_view name, f32 transition_time) {
  return SetExpression(asset::MakeAssetId(name).hash, transition_time);
}

bool ExpressionController::SetExpression(u64 pose_hash, f32 transition_time) {
  const Pose* pose = nullptr;
  for (const Pose& candidate : poses_) {
    if (candidate.name_hash == pose_hash) pose = &candidate;
  }
  if (!pose) return false;
  active_pose_ = pose_hash;

  const f32 scale = transition_time > 0 ? transition_time / kDefaultTransition : 1.0f;
  scratch_.resize(channels_.size());
  std::fill(scratch_.begin(), scratch_.end(), 0.0f);
  for (u32 i = 0; i < pose->channel.size(); ++i) scratch_[pose->channel[i]] = pose->weight[i];
  for (u32 i = 0; i < channels_.size(); ++i) {
    Channel& channel = channels_[i];
    channel.halflife = channel.base_halflife * scale;
    const f32 delay = channel.base_delay * scale;
    if (delay > 0) {
      // The channel keeps seeking its previous goal until the region's onset
      // delay elapses; the spring stays continuous through the switch.
      channel.pending_goal = scratch_[i];
      channel.pending = delay;
    } else {
      channel.goal = scratch_[i];
      channel.pending = -1;
    }
  }
  return true;
}

void ExpressionController::Update(f32 dt) {
  dt = std::max(dt, 0.0f);
  for (Channel& channel : channels_) {
    if (channel.pending >= 0) {
      channel.pending -= dt;
      if (channel.pending < 0) {
        channel.goal = channel.pending_goal;
        channel.pending = -1;
      }
    }
    Damp(&channel.value, &channel.velocity, channel.goal, channel.halflife, dt);
    channel.out = std::clamp(channel.value, 0.0f, 1.0f);
  }
}

f32 ExpressionController::Weight(u64 target_hash) const {
  for (const Channel& channel : channels_) {
    if (channel.target == target_hash) return channel.out;
  }
  return 0;
}

}  // namespace rx::anim
