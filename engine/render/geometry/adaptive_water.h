#ifndef RX_RENDER_ADAPTIVE_WATER_H_
#define RX_RENDER_ADAPTIVE_WATER_H_

#include "core/math.h"
#include "render/rhi/device.h"

namespace rx::render {

// Persistent GPU concurrent-binary-tree mesh used by one dominant planar
// ocean surface. Topology, vertices and indirect commands survive frame to
// frame; compute touches generated slots only when their leaf state changes.
class AdaptiveWaterMesh {
 public:
  struct UpdateParams {
    Mat4 local_to_clip;
    f32 bounds[4] = {0, 0, 0, 0};  // local min xz, max xz
    Vec3 camera_local;
    f32 height = 0;
    f32 time = 0;
    f32 target_pixels = 24;
    u32 render_width = 1;
    u32 render_height = 1;
    u32 triangle_budget = 16384;
    u64 surface_key = 0;
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_) && static_cast<bool>(vertices_); }

  void Update(CommandList& cmd, const UpdateParams& params);
  void Draw(CommandList& cmd) const;

  // Persistent representation for GPU consumers such as spray/foam emission
  // or water queries. The compact indirect count is counters()[1].
  const GpuBuffer& vertex_buffer() const { return vertices_; }
  const GpuBuffer& indirect_buffer() const { return commands_; }
  const GpuBuffer& counters() const { return counters_; }
  static constexpr u64 kIndirectCountOffset = sizeof(u32);

  static constexpr u32 kMaxDepth = 14;
  static constexpr u32 kMaxTriangles = 1u << (kMaxDepth + 1);  // two root trees
  static constexpr u32 SanitizeBudget(u32 requested) {
    return requested < 2 ? 2 : (requested > kMaxTriangles ? kMaxTriangles : requested);
  }

 private:
  struct DrawCommand {
    u32 vertex_count;
    u32 instance_count;
    u32 first_vertex;
    u32 first_instance;
  };

  static constexpr u32 kNodesPerTree = (1u << (kMaxDepth + 1)) - 1;
  static constexpr u32 kNodeCount = kNodesPerTree * 2;

  PipelineHandle pipeline_;
  GpuBuffer states_;
  GpuBuffer counters_;
  GpuBuffer vertices_;
  GpuBuffer commands_;
  u64 surface_key_ = ~u64{0};
};

}  // namespace rx::render

#endif  // RX_RENDER_ADAPTIVE_WATER_H_
