#ifndef RX_RENDER_WBOIT_H_
#define RX_RENDER_WBOIT_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rx::render {

class Device;

// One transparent instance for the order-independent transparency pass.
struct WboitInstance {
  Mat4 model;
  f32 color[4] = {1, 1, 1, 0.5f};  // rgb tint, a = alpha
};

// Weighted-blended order-independent transparency. Transparent geometry is
// accumulated into a colour/weight target and a transmittance target with
// commutative blends, then resolved over the opaque scene, so overlapping
// transparency needs no sorting. Owns a sphere and renders the supplied
// instances; a demo (--demo oit) of the technique that does not depend on the
// fragile material shaders.
class WboitPass {
 public:
  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);

  // Renders the instances order-independently and composites over color; returns
  // the composited colour handle.
  // Lit-translucency inputs: clustered lights plus the froxel volume's
  // transmittance (dummies when a feature is off this frame).
  struct LightingContext {
    GpuBuffer lights;
    GpuBuffer cluster_counts;
    GpuBuffer cluster_indices;
    f32 cluster_params[4] = {0, 0, 64, 64};
    TextureView froxel_volume;
    SamplerHandle froxel_sampler;
    f32 froxel_near = 0.1f;
    f32 froxel_far = 64.0f;
    bool froxel_enabled = false;
  };

  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                            const base::Vector<WboitInstance>& instances, const Mat4& view_proj,
                            const Vec3& sun_dir, const Vec3& sun_color, f32 ambient, u32 width,
                            u32 height, const LightingContext& lighting);

 private:
  PipelineHandle geom_pipeline_;
  PipelineHandle resolve_pipeline_;
  SamplerHandle sampler_;
  GpuBuffer vertices_;
  GpuBuffer indices_;
  u32 index_count_ = 0;
  Format color_format_ = Format::kUnknown;
};

}  // namespace rx::render

#endif  // RX_RENDER_WBOIT_H_
