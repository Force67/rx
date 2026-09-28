#ifndef RX_RENDER_DEPTH_OF_FIELD_H_
#define RX_RENDER_DEPTH_OF_FIELD_H_

// Bokeh depth of field: signed CoC with eased in-shader autofocus (no cpu
// readback), a half-res 48-tap golden-spiral gather, and a full-res
// composite. Runs on the AA-resolved color, before motion blur.

#include "foundation/math/math.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::render {

class DepthOfFieldPass {
 public:
  bool Initialize(gpu::Device& device);
  void Destroy(gpu::Device& device);

  struct Frame {
    f32 aperture = 6.0f;        // coc pixels per unit relative defocus
    f32 max_coc = 24.0f;        // pixels
    f32 focus_distance = 0.0f;  // meters; <= 0 enables center autofocus
    f32 focus_speed = 0.08f;
    f32 near_plane = 0.1f;
  };

  // color is the AA-resolved scene at output resolution; depth is the
  // render-res reversed-z export (uv-sampled, so resolutions may differ).
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                            gpu::Extent2D extent, const Frame& frame);

 private:
  gpu::PipelineHandle focus_pipeline_;
  gpu::PipelineHandle coc_pipeline_;
  gpu::PipelineHandle gather_pipeline_;
  gpu::PipelineHandle composite_pipeline_;
  gpu::GpuImage focus_state_;
  gpu::ResourceState focus_layout_ = gpu::ResourceState::kUndefined;
  bool focus_valid_ = false;
  gpu::SamplerHandle sampler_;
};

}  // namespace rx::render

#endif  // RX_RENDER_DEPTH_OF_FIELD_H_
