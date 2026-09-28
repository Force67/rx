#include "render/geometry/instance_store.h"

#include <float.h>
#include <math.h>

#include "base/algorithm.h"
#include "base/containers/span.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"

namespace rx::render {

namespace {

f32 MaxScale(const Mat4 &transform) {
  const f32 *m = transform.m;
  const f32 sx = ::sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const f32 sy = ::sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
  const f32 sz = ::sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
  return rx::Max(sx, rx::Max(sy, sz));
}

}  // namespace

GpuBuffer InstanceStore::Upload(Device &device, base::Span<const Mat4> transforms) {
  if (transforms.empty()) return {};
  return device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8 *>(transforms.data()), transforms.size_bytes()),
      kBufferUsageVertex);
}

void InstanceStore::ComputeBounds(Group &group, const f32 mesh_center[3], f32 mesh_radius) {
  group.cullable = mesh_radius > 0;
  group.lod_scale = 0;
  Vec3 lo{FLT_MAX, FLT_MAX,
          FLT_MAX};
  Vec3 hi{-FLT_MAX, -FLT_MAX,
          -FLT_MAX};
  for (const Mat4 &transform : group.transforms) {
    const Vec3 center = TransformPoint(transform, {mesh_center[0], mesh_center[1], mesh_center[2]});
    const f32 scale = MaxScale(transform);
    const f32 radius = mesh_radius * scale;
    group.lod_scale = rx::Max(group.lod_scale, scale);
    lo.x = rx::Min(lo.x, center.x - radius);
    lo.y = rx::Min(lo.y, center.y - radius);
    lo.z = rx::Min(lo.z, center.z - radius);
    hi.x = rx::Max(hi.x, center.x + radius);
    hi.y = rx::Max(hi.y, center.y + radius);
    hi.z = rx::Max(hi.z, center.z + radius);
  }
  group.lod_scale = rx::Max(group.lod_scale, 1e-6f);
  group.bounds_center = (lo + hi) * 0.5f;
  group.bounds_radius = 0;
  for (const Mat4 &transform : group.transforms) {
    const Vec3 center = TransformPoint(transform, {mesh_center[0], mesh_center[1], mesh_center[2]});
    const Vec3 delta = center - group.bounds_center;
    group.bounds_radius = rx::Max(
        group.bounds_radius, ::sqrtf(Dot(delta, delta)) + mesh_radius * MaxScale(transform));
  }
}

InstanceGroupHandle InstanceStore::Create(Device &device, u64 mesh,
                                          base::Span<const Mat4> transforms,
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
  group.submitted_transforms.reset();
  group.buffer = buffer;
  group.previous_buffer = {};
  group.alive = true;
  group.has_submitted_state = false;
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
                            base::Span<const Mat4> transforms, const f32 mesh_center[3],
                            f32 mesh_radius) {
  Group *group = Resolve(handle);
  if (!group || transforms.empty()) return false;
  GpuBuffer replacement = Upload(device, transforms);
  if (!replacement) return false;

  if (!group->has_submitted_state) {
    device.DestroyBufferDeferred(group->buffer);
  } else {
    const bool update_pending = static_cast<bool>(group->previous_buffer);
    const base::Vector<Mat4> &submitted =
        update_pending ? group->submitted_transforms : group->transforms;
    if (transforms.size() <= submitted.size()) {
      if (!update_pending) {
        group->submitted_transforms.assign(group->transforms.begin(), group->transforms.end());
        group->previous_buffer = group->buffer;
        group->buffer = {};
      }
    } else {
      // The previous vertex stream must cover the new draw count. Preserve the
      // submitted prefix and make newly appended instances spawn in place.
      base::Vector<Mat4> previous;
      previous.assign(transforms.begin(), transforms.end());
      base::Copy(submitted.begin(), submitted.end(), previous.begin());
      GpuBuffer previous_buffer = Upload(device, previous);
      if (!previous_buffer) {
        device.DestroyBuffer(replacement);
        return false;
      }
      if (!update_pending)
        group->submitted_transforms.assign(group->transforms.begin(), group->transforms.end());
      if (group->previous_buffer) device.DestroyBufferDeferred(group->previous_buffer);
      device.DestroyBufferDeferred(group->buffer);
      group->previous_buffer = previous_buffer;
      group->buffer = {};
    }
    if (group->buffer) device.DestroyBufferDeferred(group->buffer);
  }

  const size_t previous_count = group->transforms.size();
  group->buffer = replacement;
  group->transforms.assign(transforms.begin(), transforms.end());
  ComputeBounds(*group, mesh_center, mesh_radius);
  // Transforms changed in place: bump the revision so transform-keyed consumers
  // (the RT instance culler) re-evaluate rather than trust a stale sweep result.
  ++group->revision;
  live_instances_ = live_instances_ - previous_count + transforms.size();
  return true;
}

bool InstanceStore::Destroy(Device &device, InstanceGroupHandle handle) {
  Group *group = Resolve(handle);
  if (!group) return false;
  live_instances_ -= group->transforms.size();
  --live_groups_;
  device.DestroyBufferDeferred(group->buffer);
  if (group->previous_buffer) device.DestroyBufferDeferred(group->previous_buffer);
  group->mesh = 0;
  group->transforms.reset();
  group->submitted_transforms.reset();
  group->bounds_center = {};
  group->bounds_radius = 0;
  group->lod_scale = 1;
  group->cullable = false;
  group->alive = false;
  group->has_submitted_state = false;
  ++group->generation;
  if (group->generation == 0) ++group->generation;
  free_.push_back(handle.index);
  return true;
}

void InstanceStore::RefreshMesh(Device &device, u64 mesh, const f32 mesh_center[3], f32 mesh_radius,
                                bool compatible) {
  size_t invalidated_groups = 0;
  size_t invalidated_instances = 0;
  for (u32 index = 0; index < groups_.size(); ++index) {
    Group &group = groups_[index];
    if (!group.alive || group.mesh != mesh) continue;
    if (compatible) {
      ComputeBounds(group, mesh_center, mesh_radius);
      // The RT culler caches decisions made from the mesh-space center/radius as
      // well as transforms. A compatible re-upload can still change those
      // bounds, so retire the old visibility sweep.
      ++group.revision;
    } else {
      ++invalidated_groups;
      invalidated_instances += group.transforms.size();
      Destroy(device, {index, group.generation});
    }
  }
  if (invalidated_groups != 0) {
    RX_WARN("mesh {:x} replacement invalidated {} instance group(s) and {} instance(s)", mesh,
            invalidated_groups, invalidated_instances);
  }
}

void InstanceStore::OnFrameSubmitted(Device &device) {
  for (Group &group : groups_) {
    if (!group.alive) continue;
    if (group.previous_buffer) device.DestroyBufferDeferred(group.previous_buffer);
    group.submitted_transforms.reset();
    group.has_submitted_state = true;
  }
}

void InstanceStore::Shutdown(Device &device) {
  for (Group &group : groups_) {
    if (group.buffer) device.DestroyBuffer(group.buffer);
    if (group.previous_buffer) device.DestroyBuffer(group.previous_buffer);
  }
  groups_.clear();
  free_.clear();
  live_groups_ = 0;
  live_instances_ = 0;
}

}  // namespace rx::render
