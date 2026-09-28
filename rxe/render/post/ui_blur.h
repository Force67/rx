#ifndef RX_RENDER_UI_BLUR_H_
#define RX_RENDER_UI_BLUR_H_


#include "base/memory/unique_pointer.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::render {

// Produces a small Gaussian-blurred copy of the post-tonemap backbuffer for the
// UI's frosted-glass panels (CSS backdrop-blur). Two separable fullscreen
// passes (horizontal then vertical) at quarter resolution; the result is bound
// by the UI backend and sampled in screen space by frosted quads.
class UiBlurPass {
 public:
  static base::UniquePointer<UiBlurPass> Create(gpu::Device& device);
  ~UiBlurPass();

  UiBlurPass(const UiBlurPass&) = delete;
  UiBlurPass& operator=(const UiBlurPass&) = delete;

  // Adds the blur passes reading `src` (the backbuffer) and returns the blurred
  // texture handle. width/height are the full backbuffer dimensions.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle src, u32 width, u32 height);

  // Linear clamp sampler the UI backend uses to read the blurred result.
  gpu::SamplerHandle sampler() const { return sampler_; }

 private:
  explicit UiBlurPass(gpu::Device& device) : device_(device) {}
  void Record(PassContext& ctx, gpu::TextureView input, gpu::TextureView output, gpu::Extent2D extent,
              float dx, float dy);

  gpu::Device& device_;
  gpu::SamplerHandle sampler_;
  gpu::PipelineHandle pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_UI_BLUR_H_
