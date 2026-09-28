#ifndef RX_RENDER_ANTIALIASING_H_
#define RX_RENDER_ANTIALIASING_H_

#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/gpu/rhi/resources.h"

namespace rx::gpu {
class Device;
}  // namespace rx::gpu

namespace rx::render {


enum class AntiAliasingMode : u8 {
  kNone,
  kTaa,
  // Upscalers do their own temporal accumulation, TAA must be off when one
  // is active. The renderer enforces this.
  kUpscaler,
  // Hardware MSAA on the geometry passes (prepass + opaque scene), for people
  // who want AA without any temporal component. Guides resolve sample-0 after
  // the prepass, color averages after the scene pass, and everything
  // downstream (transparency, compositors, post) runs single-sampled exactly
  // as in kNone. No jitter; VRS and the mesh-shader path are disabled while
  // active. Sample count comes from RenderSettings::msaa_samples.
  kMsaa,
};

struct RX_RENDER_EXPORT JitterSequence {
  // Halton (2,3) offsets in pixel units, centered around zero.
  static void Sample(u32 frame_index, u32 sample_count, f32* out_x, f32* out_y);
};

// Compute resolve with reprojection and 3x3 neighborhood clamping. Owns two
// persistent history images and ping pongs between them: the one written
// this frame is both the resolved output and next frame's history.
class TaaPass {
 public:
  struct Settings {
    f32 history_blend = 0.9f;
    u32 jitter_sample_count = 8;
  };

  bool Initialize(gpu::Device& device);
  void Resize(gpu::Device& device, gpu::Extent2D extent);
  void Destroy(gpu::Device& device);

  void Configure(const Settings& settings) { settings_ = settings; }
  void Reset() { history_valid_ = false; }

  // Adds the resolve pass and returns the handle of the resolved output.
  // debug_mode replaces the output with a debug visualization to a side target:
  // 1 = disocclusion heatmap (history rejection), 2 = motion vectors.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle motion,
                            u32 frame_index, u32 debug_mode = 0);

  const Settings& settings() const { return settings_; }

 private:
  Settings settings_;
  gpu::SamplerHandle sampler_;
  gpu::PipelineHandle pipeline_;
  gpu::GpuImage history_[2];
  gpu::ResourceState history_states_[2] = {gpu::ResourceState::kUndefined, gpu::ResourceState::kUndefined};
  gpu::Extent2D extent_{};
  bool history_valid_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_ANTIALIASING_H_
