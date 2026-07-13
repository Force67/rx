#ifndef RX_RENDER_RCGI_H_
#define RX_RENDER_RCGI_H_

#include <memory>

#include "core/math.h"
#include "render/core/bindless.h"
#include "render/core/render_graph.h"
#include "render/gi/light_grid.h"
#include "render/rhi/device.h"

namespace rx::render {

class RayTracingContext;

// RCGI world side (idTech8-style radiance-cached GI, SIGGRAPH 2025). Owns the
// cascaded irradiance/visibility probe atlases, the spatially-hashed world
// radiance cache, and the passes that fill them: probe trace -> args -> cache
// shade (indirect) -> blend -> border. A separate M1 resolve pass samples the
// cascades per screen pixel (DDGI-equivalent quality) into a full-res
// irradiance texture the forward pass consumes; M2 replaces that filler with
// the gather/denoise/upscale chain writing the same texture.
//
// Modeled on DdgiSystem: resource creation, camera snapping, hysteresis and
// barrier discipline follow it. The world cache and light-grid shading are the
// additions. See shaders/gi/rcgi_common.hlsli for the M2-facing interface.
class RcgiSystem {
 public:
  static constexpr u32 kProbesPerAxis = 16;
  static constexpr u32 kCascades = 4;
  static constexpr u32 kIrradianceTexels = 8;
  static constexpr u32 kVisibilityTexels = 8;
  static constexpr u32 kRaysPerProbe = 32;
  static constexpr f32 kBaseSpacing = 2.0f;  // meters, cascade 0 (doubles per cascade)
  static constexpr f32 kBaseCell = 0.25f;    // radiance-cache base cell (meters)
  static constexpr f32 kLodDistance = 8.0f;  // radiance-cache distance-LOD scale (meters)
  static constexpr u32 kHashCapacity = 1u << 20;
  static constexpr u32 kActiveCapacity = 1u << 18;
  static constexpr u32 kEntryStride = 12;  // u32 per hash slot (rcgi_common.hlsli)

  struct Settings {
    f32 hysteresis = 0.97f;
    f32 energy_scale = 1.0f;
  };

  static std::unique_ptr<RcgiSystem> Create(Device& device, TextureView sky_view,
                                            SamplerHandle sky_sampler, BindlessRegistry& bindless);
  ~RcgiSystem();

  RcgiSystem(const RcgiSystem&) = delete;
  RcgiSystem& operator=(const RcgiSystem&) = delete;

  void Configure(const Settings& settings) { settings_ = settings; }

  // World-side update for the current frame's cascade (round-robin frame % 4):
  // snaps the cascade, uploads the globals UBO, and records trace/args/shade/
  // blend/border. `light_grid` must have been updated earlier in the graph;
  // `lights` is the same StructuredBuffer<Light> the cluster/light-grid consume.
  void AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                  const LightGrid& light_grid, const GpuBuffer& lights, const Vec3& camera,
                  const Vec3& sun_direction, f32 sun_intensity, const Vec3& sun_color,
                  u32 frame_index, bool async = false);

  // M1 filler: full-res irradiance from the cascades, sampled per screen pixel.
  // Reads the prepass depth + oct normals; returns the "rcgi_irradiance"
  // transient the forward pass reads (multiplied by `intensity`).
  ResourceHandle AddResolvePass(RenderGraph& graph, ResourceHandle depth_export,
                                ResourceHandle normals, Extent2D extent, const Mat4& inv_view_proj,
                                const Vec3& camera, f32 intensity, u32 frame_index);

  // Zero the hash on the next update (camera teleports / big jumps).
  void RequestReset() { clear_hash_ = true; }

 private:
  struct RcgiGlobals {
    f32 cascade_origin[kCascades][4];  // xyz origin, w probe spacing
    f32 camera_pos[4];                 // xyz eye, w LOD distance
    f32 sun_direction[4];              // xyz dir, w intensity
    f32 sun_color[4];                  // rgb, w rays per probe
    u32 counts[4];                     // probes x,y,z, irradiance texels
    u32 misc[4];                       // x current cascade, y frame, z cascades, w hash capacity
    f32 params[4];                     // x max ray dist, y hysteresis, z energy, w base cell
  };

  explicit RcgiSystem(Device& device) : device_(device) {}
  bool CreateResources();
  bool CreatePipelines();
  Vec3 SnapOrigin(const Vec3& camera, u32 cascade) const;

  Device& device_;
  Settings settings_;
  SamplerHandle sampler_;
  TextureView sky_view_;
  SamplerHandle sky_sampler_;
  BindlessRegistry* bindless_ = nullptr;

  GpuImage irradiance_;   // rgba16f cascade atlas (4 stacked slabs)
  GpuImage visibility_;   // rgba16f moments atlas (rg = mean, mean^2)
  GpuImage rays_;         // rgba16f: rgb sky (miss) / signed hit distance in a
  GpuBuffer state_;       // hash slots: kEntryStride u32 each
  GpuBuffer radiance_;    // packed rgba16f (uint2) per slot
  GpuBuffer active_list_; // per-frame active hash indices
  GpuBuffer active_meta_; // [0] = active count
  GpuBuffer dispatch_args_;      // indirect args for cache shade
  GpuBuffer globals_buffers_[2]; // host visible, ping-pong by frame parity

  PipelineHandle probe_trace_pipeline_;
  PipelineHandle args_pipeline_;
  PipelineHandle cache_shade_pipeline_;
  PipelineHandle blend_pipeline_;
  PipelineHandle border_pipeline_;
  PipelineHandle resolve_pipeline_;

  bool atlas_initialized_ = false;
  bool history_valid_ = false;
  bool clear_hash_ = true;  // first update zeroes the hash
  Vec3 blended_origin_[kCascades] = {};
  Vec3 last_camera_{};
};

}  // namespace rx::render

#endif  // RX_RENDER_RCGI_H_
