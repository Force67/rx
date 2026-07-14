#ifndef RX_RENDER_INSTANCE_STORE_H_
#define RX_RENDER_INSTANCE_STORE_H_

#include <base/containers/vector.h>

#include <span>

#include "core/math.h"
#include "render/rhi/device.h"

namespace rx::render {

// Generation-checked handle to one immutable, mesh-homogeneous instance group.
// Groups are intended to follow a streaming unit such as a world cell, so they
// are created and destroyed infrequently while each frame only walks groups.
struct InstanceGroupHandle {
  u32 index = ~0u;
  u32 generation = 0;

  explicit operator bool() const { return index != ~0u; }
};

class InstanceStore {
 public:
  struct Group {
    u64 mesh = 0;
    base::Vector<Mat4> transforms;
    GpuBuffer buffer;
    Vec3 bounds_center{};
    f32 bounds_radius = 0;
    f32 lod_scale = 1;
    bool cullable = false;
    u32 generation = 1;
    bool alive = false;
  };

  InstanceGroupHandle Create(Device &device, u64 mesh, std::span<const Mat4> transforms,
                             const f32 mesh_center[3], f32 mesh_radius);
  bool Replace(Device &device, InstanceGroupHandle handle, std::span<const Mat4> transforms,
               const f32 mesh_center[3], f32 mesh_radius);
  bool Destroy(Device &device, InstanceGroupHandle handle);
  void RefreshMesh(Device &device, u64 mesh, const f32 mesh_center[3], f32 mesh_radius,
                   bool compatible);
  void Shutdown(Device &device);

  const base::Vector<Group> &groups() const { return groups_; }
  size_t group_count() const { return live_groups_; }
  size_t instance_count() const { return live_instances_; }

 private:
  Group *Resolve(InstanceGroupHandle handle);
  static void ComputeBounds(Group &group, const f32 mesh_center[3], f32 mesh_radius);
  static GpuBuffer Upload(Device &device, std::span<const Mat4> transforms);

  base::Vector<Group> groups_;
  base::Vector<u32> free_;
  size_t live_groups_ = 0;
  size_t live_instances_ = 0;
};

}  // namespace rx::render

#endif  // RX_RENDER_INSTANCE_STORE_H_
