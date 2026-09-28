#ifndef RX_RENDER_AMBIENT_OCCLUSION_H_
#define RX_RENDER_AMBIENT_OCCLUSION_H_

#include "foundation/math/math.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/gpu/rhi/resources.h"

namespace rx::gpu {
class Device;
}  // namespace rx::gpu

namespace rx::render {

class RayTracingContext;

// Ray traced ambient occlusion: cosine hemisphere rays through the frame TLAS,
// producing a raw normalized hit distance packed for NRD's REBLUR occlusion
// denoiser (which owns all temporal/spatial filtering). Needs ray query
// support; the renderer skips it otherwise.
class RtaoPass {
 public:
  struct Settings {
    f32 radius = 1.2f;  // meters
    u32 ray_count = 2;
  };

  bool Initialize(gpu::Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }
  void Resize(gpu::Device& device, gpu::Extent2D extent) { extent_ = extent; }
  void Destroy(gpu::Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }

  // Adds the trace pass and returns the packed normalized hit distance (R8).
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            ResourceHandle depth, ResourceHandle normals,
                            const Mat4& inv_view_proj, u32 frame_index, f32 near_plane,
                            const f32 hit_dist_params[3]);

  static constexpr gpu::Format kHitDistFormat = gpu::Format::kR8Unorm;

 private:
  Settings settings_;
  gpu::PipelineHandle pipeline_;
  gpu::Extent2D extent_{};
};

}  // namespace rx::render

#endif  // RX_RENDER_AMBIENT_OCCLUSION_H_
