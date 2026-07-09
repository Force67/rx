#ifndef RX_RENDER_VRS_RATE_H_
#define RX_RENDER_VRS_RATE_H_

// Content-adaptive variable rate shading: a persistent rate image (one texel
// per caps().shading_rate_texel block) attached to the scene pass, rebuilt
// each frame from the lit color's luminance detail and screen motion. The
// scene consumes last frame's rates - flat, dark or fast-moving regions
// shade at 2x1/1x2/2x2 (4x4 where supported) with no visible loss under TAA.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class VrsRatePass {
 public:
  bool Initialize(Device& device);
  void Destroy(Device& device);
  // (Re)creates the rate image for a render resolution. Safe to call every
  // resize; no-ops when the size is unchanged.
  bool Resize(Device& device, Extent2D render_extent);
  bool available() const { return static_cast<bool>(pipeline_) && static_cast<bool>(rate_); }

  // Records the rate rebuild from this frame's lit color + motion. The rate
  // image leaves the pass back in kShadingRate for next frame's scene pass.
  void AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle motion,
                  Extent2D render_extent, f32 threshold, f32 motion_scale);

  // Attachment for RenderingInfo::shading_rate (always in kShadingRate
  // outside AddToGraph's pass).
  TextureView rate_view() const { return rate_.view; }

 private:
  PipelineHandle pipeline_;
  GpuImage rate_;
  SamplerHandle sampler_;
  Extent2D render_extent_ = {0, 0};
  u32 texel_size_ = 16;
  bool allow_coarse_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_VRS_RATE_H_
