#ifndef RX_RENDER_PRECIPITATION_H_
#define RX_RENDER_PRECIPITATION_H_

#include "foundation/math/math.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/gpu/rhi/resources.h"

namespace rx::gpu {
class Device;
}  // namespace rx::gpu

namespace rx::render {


// Screen-space precipitation (rain streaks / snow flakes) composited over the
// lit scene, driven by the weather system. Procedural, world-anchored, cheap; no
// particle simulation. Runs every frame when intensity > 0.
class Precipitation {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    f32 time = 0.0f;     // seconds, drives the fall animation
    f32 intensity = 0.0f;  // 0 none .. 1 heavy
    bool snow = false;     // snow flakes vs rain streaks
  };

  bool Initialize(gpu::Device& device);
  void Destroy(gpu::Device& device);

  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, gpu::Extent2D extent,
                            const Frame& frame);

 private:
  gpu::PipelineHandle pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_PRECIPITATION_H_
