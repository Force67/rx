#ifndef RX_RENDER_WATER_H_
#define RX_RENDER_WATER_H_


#include "base/memory/unique_pointer.h"
#include "rxe/render/core/bindless.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/render/geometry/adaptive_water.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::render {

// Water surface pipeline: fbm wave normals, raytraced reflections shaded
// through the bindless tables, screen space refraction with absorption
// against a snapshot of the opaque pass. Needs ray query; without it water
// falls back to the generic blend pipeline upstream.
class WaterPass {
 public:
  // Set layouts mirror the water.ps bindings: 0 mesh globals (+tlas),
  // 1 material, 2 environment, 3 bindless, 4 the opaque snapshot.
  static base::UniquePointer<WaterPass> Create(gpu::Device& device, gpu::Format color_format,
                                           gpu::Format motion_format, gpu::Format depth_format,
                                           gpu::BindingLayoutHandle globals_layout,
                                           gpu::BindingLayoutHandle material_layout,
                                           gpu::BindingLayoutHandle environment_layout,
                                           gpu::BindingLayoutHandle bindless_layout);
  ~WaterPass();

  WaterPass(const WaterPass&) = delete;
  WaterPass& operator=(const WaterPass&) = delete;

  // Compute copy of the opaque scene color into the snapshot transient.
  void RecordCopy(PassContext& ctx, ResourceHandle scene_color, ResourceHandle opaque_color,
                  u32 width, u32 height);

  // Binds the water pipeline; sets 0-2 are the frame's globals/material-less
  // environment sets, set 4 is written from the snapshot views.
  void Bind(PassContext& ctx, gpu::BindingSetHandle globals, gpu::BindingSetHandle environment,
            gpu::BindingSetHandle bindless, ResourceHandle opaque_color, ResourceHandle opaque_depth);
  void BindMaterial(gpu::CommandList& cmd, gpu::BindingSetHandle material);

  bool adaptive_available() const { return adaptive_.available(); }
  void UpdateAdaptive(gpu::CommandList& cmd, const AdaptiveWaterMesh::UpdateParams& params) {
    adaptive_.Update(cmd, params);
  }
  void DrawAdaptive(gpu::CommandList& cmd) const { adaptive_.Draw(cmd); }

 private:
  explicit WaterPass(gpu::Device& device) : device_(device) {}

  gpu::Device& device_;
  gpu::SamplerHandle sampler_;
  gpu::PipelineHandle pipeline_;
  gpu::PipelineHandle copy_pipeline_;
  AdaptiveWaterMesh adaptive_;
};

}  // namespace rx::render

#endif  // RX_RENDER_WATER_H_
