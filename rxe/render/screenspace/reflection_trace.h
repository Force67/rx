#ifndef RX_RENDER_REFLECTION_TRACE_H_
#define RX_RENDER_REFLECTION_TRACE_H_

// Stochastic specular reflections: one VNDF-sampled GGX ray per pixel through
// the TLAS, hit-shaded like the forward rt variant's inline mirror ray (plus
// a sun shadow ray), packed for NRD REBLUR_SPECULAR. The denoised result is
// sampled by the forward pass instead of tracing inline, which turns the
// mirror-to-IBL crossfade into a real glossy distribution.

#include "foundation/build_config/export.h"
#include "foundation/math/math.h"
#include "rxe/render/core/render_graph.h"
#include "rxe/gpu/rhi/device.h"

namespace rx::render {

class RayTracingContext;

class RX_RENDER_EXPORT ReflectionTrace {
 public:
  struct Frame {
    Mat4 inv_view_proj;  // unjittered
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel direction
    f32 sun_intensity = 1.0f;
    Vec3 sun_color{1.0f, 1.0f, 1.0f};
    f32 roughness_cutoff = 0.6f;
    u32 frame_index = 0;
    f32 near_plane = 0.1f;
    const f32* hit_dist_params = nullptr;  // NrdDenoiser::kHitDistParams
    bool ddgi = false;
    // Evaluate the real alpha texture on masked (vegetation) hits via a bounded
    // any-hit loop (RX_RT_VEG_ANYHIT). Off = the old force-opaque approximation.
    bool veg_anyhit = false;
    // Trace + light at half resolution, then bilateral-upscale to full res before
    // NRD (RX_REFL_HALF). The dominant reflection cost is the TLAS ray; this
    // quarters the ray count.
    bool half_res = false;
    // Roughness-scaled reflection reach: maxDist*((1-r)^2+0.1) (AC Shadows).
    f32 max_ray_dist = 200.0f;
    // One-step exponential-height-fog on hits (RX_REFL_FOG); applied only when
    // fog_density > 0 (i.e. the raster fog is active), so it stays consistent.
    bool fog = false;
    f32 fog_density = 0.0f;
    f32 fog_height_falloff = 0.0f;
    f32 fog_base_height = 0.0f;
    // Specular ray-skip: on rough / off-mirror rays evaluate the RCGI per-pixel
    // diffuse SH instead of tracing (RX_REFL_SH_SKIP). Needs the gather SH bound
    // (sh_* handles valid); ignored otherwise.
    bool sh_skip = false;
    f32 sh_skip_roughness = 0.45f;
    f32 sh_dir_threshold = 0.5f;
  };

  // RCGI irradiance-cascade resources for the spec-bounce indirect term. When
  // `active`, the diffuse GI at a reflection hit reads the RCGI cascades
  // (kFlagRcgi) instead of the DDGI atlas, which is empty under RCGI. The
  // renderer always supplies valid handles (the real RCGI images when the
  // system is up, otherwise environment placeholders), so the descriptor set is
  // always complete; `active` alone gates the sampling. `in_general` marks the
  // atlases as living in kGeneral (true only for the real RCGI images).
  struct RcgiBinding {
    gpu::TextureView irradiance{};
    gpu::TextureView visibility{};
    const gpu::GpuBuffer* globals = nullptr;
    const gpu::GpuBuffer* probe_meta = nullptr;
    const gpu::GpuBuffer* interior_vols = nullptr;
    gpu::SamplerHandle sampler{};
    bool in_general = false;
    bool active = false;
  };

  bool Initialize(gpu::Device& device, gpu::BindingLayoutHandle bindless_layout);
  bool available() const { return pipeline_ && upscale_pipeline_; }
  void Destroy(gpu::Device& device);

  // Returns the full-res packed radiance+hitdist target (rgba16f) for
  // DenoiseSpecular. When frame.half_res the trace runs at half `extent` and an
  // extra bilateral upscale pass reconstructs full res. sh_r/g/b are the RCGI
  // gather's denoised per-pixel SH (kInvalidResource when RCGI is off); sh_extent
  // is their (gather) resolution.
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            gpu::BindingSetHandle bindless_set, ResourceHandle depth,
                            ResourceHandle normals, gpu::TextureView prefiltered,
                            gpu::TextureView ddgi_irradiance, bool ddgi_in_general,
                            const gpu::GpuBuffer& ddgi_volume, u64 ddgi_volume_size,
                            gpu::SamplerHandle sampler, gpu::Extent2D extent, ResourceHandle sh_r,
                            ResourceHandle sh_g, ResourceHandle sh_b, gpu::Extent2D sh_extent,
                            const RcgiBinding& rcgi, const Frame& frame);

 private:
  gpu::PipelineHandle pipeline_;
  gpu::PipelineHandle upscale_pipeline_;  // half-res -> full-res bilateral upscale
  gpu::GpuBuffer camera_[2];  // inv_view_proj + eye, too big for the push block
};

}  // namespace rx::render

#endif  // RX_RENDER_REFLECTION_TRACE_H_
