#ifndef RX_RENDER_FLUID_SURFACE_H_
#define RX_RENDER_FLUID_SURFACE_H_

#include <memory>

#include "render/core/render_graph.h"
#include "render/geometry/fluid_sim.h"
#include "render/rhi/device.h"

namespace rx::render {

// Surface renderer for the heightfield fluid solver (see fluid_sim.h): one
// procedural displaced-grid draw over the active domain, in the transparent
// phase, that samples the solver's state/bed/velocity textures and shades a
// flowing water surface and a glowing lava surface.
//
// Geometry is proceduralised: a single non-indexed Draw of 6*N*N vertices with
// instance_count = 2 (N = min(domain.resolution, 512)). SV_VertexID decodes the
// cell (i,j) + quad corner; SV_InstanceID selects the fluid (0 water, 1 lava).
// The VS maps cell -> world XZ from the params CB, samples bed + state, and
// lifts the vertex to the fluid surface height, sinking dry cells to a
// degenerate (clipped) position so empty regions cost nothing.
//
// Set layout mirrors the water surface conventions: set 0 = mesh frame globals
// (view_proj, camera, sun, time), set 2 = the environment IBL set (space2 in
// HLSL), and set 1 = one transient set of our own (state/bed/velocity combined
// samplers + the solver's params CB). Blends alpha over the scene (no refraction
// snapshot needed: shallow water is see-through for free) and writes depth so
// water-over-lava resolves regardless of raster order.
class FluidSurfacePass {
 public:
  // Attachments mirror the transparent pass (scene color + motion + depth); the
  // formats/layouts come straight from the WaterPass::Create call site so the
  // render pass stays compatible. bindless_layout is accepted for call-site
  // parity with the other transparent passes but this pass needs no bindless
  // table (IBL comes through the environment set).
  static std::unique_ptr<FluidSurfacePass> Create(Device& device, Format color_format,
                                                  Format motion_format, Format depth_format,
                                                  BindingLayoutHandle globals_layout,
                                                  BindingLayoutHandle environment_layout,
                                                  BindingLayoutHandle bindless_layout);
  ~FluidSurfacePass();

  FluidSurfacePass(const FluidSurfacePass&) = delete;
  FluidSurfacePass& operator=(const FluidSurfacePass&) = delete;

  // Binds the pipeline, the frame globals (set 0) + environment (set 2) sets and
  // a transient set 1 wrapping the solver's GENERAL-layout images + params CB,
  // then issues the grid draw for the frame's domain. Caller guarantees the sim
  // is active and stepped this frame (its final barrier makes the state readable
  // in the graphics stages).
  void Draw(PassContext& ctx, BindingSetHandle globals, BindingSetHandle environment,
            const FluidSim& sim, u32 frame_slot, f32 time);

 private:
  explicit FluidSurfacePass(Device& device) : device_(device) {}

  Device& device_;
  SamplerHandle sampler_;
  PipelineHandle pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_FLUID_SURFACE_H_
