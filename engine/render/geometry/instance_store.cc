#include "render/geometry/instance_store.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace rx::render {

namespace {

f32 MaxScale(const Mat4 &transform) {
  const f32 *m = transform.m;
  const f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
  const f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
  return std::max(sx, std::max(sy, sz));
}

}  // namespace

GpuBuffer InstanceStore::Upload(Device &device, std::span<const Mat4> transforms) {
  if (transforms.empty()) return {};
  GpuBuffer buffer =
      device.CreateBuffer(static_cast<u64>(transforms.size_bytes()), kBufferUsageVertex, true);
  if (!buffer.mapped) {
    if (buffer) device.DestroyBuffer(buffer);
    return {};
  }
  std::memcpy(buffer.mapped, transforms.data(), transforms.size_bytes());
  return buffer;
}

void InstanceStore::ComputeBounds(Group &group, const f32 mesh_center[3], f32 mesh_radius) {
  group.cullable = mesh_radius > 0;
  group.lod_scale = 0;
  Vec3 lo{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
          std::numeric_limits<f32>::max()};
  Vec3 hi{-std::numeric_limits<f32>::max(), -std::numeric_limits<f32>::max(),
          -std::numeric_limits<f32>::max()};
  for (const Mat4 &transform : group.transforms) {
    const Vec3 center = TransformPoint(transform, {mesh_center[0], mesh_center[1], mesh_center[2]});
    const f32 scale = MaxScale(transform);
    const f32 radius = mesh_radius * scale;
    group.lod_scale = std::max(group.lod_scale, scale);
    lo.x = std::min(lo.x, center.x - radius);
    lo.y = std::min(lo.y, center.y - radius);
    lo.z = std::min(lo.z, center.z - radius);
    hi.x = std::max(hi.x, center.x + radius);
    hi.y = std::max(hi.y, center.y + radius);
    hi.z = std::max(hi.z, center.z + radius);
  }
  group.lod_scale = std::max(group.lod_scale, 1e-6f);
  group.bounds_center = (lo + hi) * 0.5f;
  group.bounds_radius = 0;
  for (const Mat4 &transform : group.transforms) {
    const Vec3 center = TransformPoint(transform, {mesh_center[0], mesh_center[1], mesh_center[2]});
    const Vec3 delta = center - group.bounds_center;
    group.bounds_radius = std::max(
        group.bounds_radius, std::sqrt(Dot(delta, delta)) + mesh_radius * MaxScale(transform));
  }
}

InstanceGroupHandle InstanceStore::Create(Device &device, u64 mesh,
                                          std::span<const Mat4> transforms,
                                          const f32 mesh_center[3], f32 mesh_radius) {
  if (mesh == 0 || transforms.empty()) return {};
  GpuBuffer buffer = Upload(device, transforms);
  if (!buffer) return {};

  u32 index;
  if (free_.empty()) {
    index = static_cast<u32>(groups_.size());
    groups_.push_back({});
  } else {
    index = free_.back();
    free_.pop_back();
  }
  Group &group = groups_[index];
  group.mesh = mesh;
  group.transforms.assign(transforms.begin(), transforms.end());
  group.buffer = buffer;
  group.alive = true;
  ComputeBounds(group, mesh_center, mesh_radius);
  ++live_groups_;
  live_instances_ += transforms.size();
  return {index, group.generation};
}

InstanceStore::Group *InstanceStore::Resolve(InstanceGroupHandle handle) {
  if (handle.index >= groups_.size()) return nullptr;
  Group &group = groups_[handle.index];
  return group.alive && group.generation == handle.generation ? &group : nullptr;
}

bool InstanceStore::Replace(Device &device, InstanceGroupHandle handle,
                            std::span<const Mat4> transforms, const f32 mesh_center[3],
                            f32 mesh_radius) {
  Group *group = Resolve(handle);
  if (!group || transforms.empty()) return false;
  GpuBuffer replacement = Upload(device, transforms);
  if (!replacement) return false;
  const size_t previous_count = group->transforms.size();
  device.DestroyBufferDeferred(group->buffer);
  group->buffer = replacement;
  group->transforms.assign(transforms.begin(), transforms.end());
  ComputeBounds(*group, mesh_center, mesh_radius);
  live_instances_ = live_instances_ - previous_count + transforms.size();
  return true;
}

bool InstanceStore::Destroy(Device &device, InstanceGroupHandle handle) {
  Group *group = Resolve(handle);
  if (!group) return false;
  live_instances_ -= group->transforms.size();
  --live_groups_;
  device.DestroyBufferDeferred(group->buffer);
  group->mesh = 0;
  group->transforms.reset();
  group->bounds_center = {};
  group->bounds_radius = 0;
  group->lod_scale = 1;
  group->cullable = false;
  group->alive = false;
  ++group->generation;
  if (group->generation == 0) ++group->generation;
  free_.push_back(handle.index);
  return true;
}

void InstanceStore::RefreshMesh(Device &device, u64 mesh, const f32 mesh_center[3], f32 mesh_radius,
                                bool compatible) {
  for (u32 index = 0; index < groups_.size(); ++index) {
    Group &group = groups_[index];
    if (!group.alive || group.mesh != mesh) continue;
    if (compatible) {
      ComputeBounds(group, mesh_center, mesh_radius);
    } else {
      Destroy(device, {index, group.generation});
    }
  }
}

void InstanceStore::Shutdown(Device &device) {
  for (Group &group : groups_) {
    if (group.buffer) device.DestroyBuffer(group.buffer);
  }
  groups_.clear();
  free_.clear();
  live_groups_ = 0;
  live_instances_ = 0;
}

}  // namespace rx::render
