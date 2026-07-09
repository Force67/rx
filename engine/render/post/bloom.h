#ifndef RX_RENDER_BLOOM_H_
#define RX_RENDER_BLOOM_H_

#include "core/types.h"
#include "render/core/render_graph.h"

namespace rx::render {

class Device;

// Threshold-free physically based bloom: 13-tap Karis downsample chain into
// tent upsamples (CoD: Advanced Warfare). The tonemap pass mixes the result
// by a small weight instead of clipping a threshold.
class BloomPass {
 public:
  static constexpr u32 kMips = 6;

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Builds the chain off `input` (output resolution hdr) and returns the
  // full resolution bloom texture. If `flare_src` is non-null it receives a
  // snapshot of the 1/4-res down-chain mip, taken before the up chain widens
  // it in place: a tight prefiltered highlight source for the lens-flare
  // ghosts (ghosts sampled from the final wide bloom smear into a screen-wide
  // halo).
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle input, u32 width, u32 height,
                            ResourceHandle* flare_src = nullptr);

 private:
  SamplerHandle sampler_;
  PipelineHandle down_pipeline_;
  PipelineHandle up_pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_BLOOM_H_
