#ifndef RX_RENDER_MOTION_BLUR_H_
#define RX_RENDER_MOTION_BLUR_H_

// Per-object + camera motion blur (McGuire tile-max gather): a 16px tile map
// of dominant velocities, then a velocity-weighted gather along the 3x3 tile
// neighborhood's max. Consumes the prepass motion target (works at output
// resolution after the AA/upscale resolve; uv velocities are resolution
// independent).

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class MotionBlurPass {
 public:
  bool Initialize(Device& device);
  void Destroy(Device& device);

  struct Frame {
    f32 shutter = 0.5f;      // fraction of frame the shutter is open (180deg = 0.5)
    f32 max_blur_px = 48.0f; // clamp streak length, pixels
    u32 samples = 10;
    u32 frame_index = 0;
    // Debug: overrides every velocity with a uniform screen-space vector
    // (pixels) so the gather path is testable from a static camera.
    f32 debug_velocity[2] = {0.0f, 0.0f};
  };

  // Returns the blurred color target (same size as input).
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle motion,
                            Extent2D color_extent, const Frame& frame);

 private:
  PipelineHandle tilemax_pipeline_;
  PipelineHandle blur_pipeline_;
  SamplerHandle sampler_;
};

}  // namespace rx::render

#endif  // RX_RENDER_MOTION_BLUR_H_
