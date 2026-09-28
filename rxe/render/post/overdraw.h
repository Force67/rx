#ifndef RX_RENDER_OVERDRAW_H_
#define RX_RENDER_OVERDRAW_H_


#include "base/functional/function.h"
#include "foundation/math/math.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::render {

// Overdraw debug view. Re-renders the scene geometry with additive blending and
// no depth test into the resolved color (cleared first), so overlapping layers
// accumulate into a warm heat ramp. Reuses shadow.vs for the mvp transform; the
// caller emits the same opaque + transparent draws it would otherwise shade.
class OverdrawPass {
 public:
  bool Initialize(gpu::Device& device, gpu::Format color_format);
  void Destroy(gpu::Device& device);

  // Clears `color_view` and additive-renders the geometry. draw is invoked with
  // the pipeline bound; for each mesh it pushes the full {view_proj, model}
  // 128-byte block (push constants always start at offset 0 in the RHI) and
  // issues the draws. view_proj is also pushed up front.
  void Render(gpu::CommandList& cmd, gpu::TextureView color_view, gpu::Extent2D extent, const Mat4& view_proj,
              const base::Function<void(gpu::CommandList&)>& draw);
  void BindInstanced(gpu::CommandList& cmd, const Mat4& view_proj);

 private:
  gpu::PipelineHandle pipeline_;
  gpu::PipelineHandle instanced_pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_OVERDRAW_H_
