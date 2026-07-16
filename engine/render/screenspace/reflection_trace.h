#ifndef RX_RENDER_REFLECTION_TRACE_H_
#define RX_RENDER_REFLECTION_TRACE_H_

// Stochastic specular reflections: one VNDF-sampled GGX ray per pixel through
// the TLAS, hit-shaded like the forward rt variant's inline mirror ray (plus
// a sun shadow ray), packed for NRD REBLUR_SPECULAR. The denoised result is
// sampled by the forward pass instead of tracing inline, which turns the
// mirror-to-IBL crossfade into a real glossy distribution.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class RayTracingContext;

class ReflectionTrace {
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
  // renderer always supplies valid handles -- the real RCGI images when the
  // system is up, otherwise environment placeholders -- so the descriptor set is
  // always complete; `active` alone gates the sampling. `in_general` marks the
  // atlases as living in kGeneral (true only for the real RCGI images).
  struct RcgiBinding {
    TextureView irradiance{};
    TextureView visibility{};
    const GpuBuffer* globals = nullptr;
    const GpuBuffer* probe_meta = nullptr;
    const GpuBuffer* interior_vols = nullptr;
    SamplerHandle sampler{};
    bool in_general = false;
    bool active = false;
  };

  bool Initialize(Device& device, BindingLayoutHandle bindless_layout);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  // Returns the full-res packed radiance+hitdist target (rgba16f) for
  // DenoiseSpecular. When frame.half_res the trace runs at half `extent` and an
  // extra bilateral upscale pass reconstructs full res. sh_r/g/b are the RCGI
  // gather's denoised per-pixel SH (kInvalidResource when RCGI is off); sh_extent
  // is their (gather) resolution.
  ResourceHandle AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            BindingSetHandle bindless_set, ResourceHandle depth,
                            ResourceHandle normals, TextureView prefiltered,
                            TextureView ddgi_irradiance, bool ddgi_in_general,
                            const GpuBuffer& ddgi_volume, u64 ddgi_volume_size,
                            SamplerHandle sampler, Extent2D extent, ResourceHandle sh_r,
                            ResourceHandle sh_g, ResourceHandle sh_b, Extent2D sh_extent,
                            const RcgiBinding& rcgi, const Frame& frame);

 private:
  PipelineHandle pipeline_;
  PipelineHandle upscale_pipeline_;  // half-res -> full-res bilateral upscale
};

}  // namespace rx::render

#endif  // RX_RENDER_REFLECTION_TRACE_H_
