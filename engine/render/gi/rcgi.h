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
  // transient the forward pass reads (multiplied by `intensity`). Retained as the
  // `RX_RCGI_PROBES_ONLY=1` A/B fallback for the M2 gather chain.
  ResourceHandle AddResolvePass(RenderGraph& graph, ResourceHandle depth_export,
                                ResourceHandle normals, Extent2D extent, const Mat4& inv_view_proj,
                                const Vec3& camera, f32 intensity, u32 frame_index);

  // M2 screen side: half-res SH final gather -> separable bilateral denoise ->
  // full-res poisson upscale + temporal filter, writing the same
  // "rcgi_irradiance" transient (env set 2 slot 35) the M1 resolve produced.
  // Consumes the prepass depth/normals/motion, the frame TLAS, and the world
  // cache/atlas resources this system owns. `reset` drops temporal + screen-cache
  // history (first frame / resize / teleport).
  ResourceHandle AddGatherChain(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                                ResourceHandle depth_export, ResourceHandle normals,
                                ResourceHandle motion, Extent2D extent, const Mat4& inv_view_proj,
                                const Mat4& prev_view_proj, const Vec3& camera, f32 intensity,
                                u32 frame_index, bool reset);

  // End-of-frame snapshot of the lit HDR scene colour + depth into the persistent
  // screen-cache history the next frame's gather samples. Record only when RCGI
  // is active (zero cost otherwise).
  void AddHistoryCopy(RenderGraph& graph, ResourceHandle lit_color, ResourceHandle depth_export,
                      Extent2D extent);

  // (Re)create the render-resolution screen-side history images when the extent
  // changes. No-op if already sized. Must be called before AddGatherChain.
  // Returns false if (re)creation failed: in that case nothing is left imported
  // and the caller must skip the gather chain (fall back to the probes-only
  // resolve) for this frame; the cleared extent lets a later frame retry.
  bool EnsureScreenResources(Extent2D extent);

  // Zero the hash on the next update (camera teleports / big jumps).
  void RequestReset() {
    clear_hash_ = true;
    screen_history_valid_ = false;
  }

 private:
  struct RcgiGlobals {
    f32 cascade_origin[kCascades][4];  // xyz origin, w probe spacing
    f32 camera_pos[4];                 // xyz eye, w LOD distance
    f32 sun_direction[4];              // xyz dir, w intensity
    f32 sun_color[4];                  // rgb, w rays per probe
    u32 counts[4];                     // probes x,y,z, irradiance texels
    u32 misc[4];                       // x current cascade, y frame, z cascades, w hash capacity
    f32 params[4];                     // x max ray dist, y hysteresis, z energy, w base cell
    u32 valid[4];                      // x per-cascade "blended since (re)creation" bitmask
  };

  explicit RcgiSystem(Device& device) : device_(device) {}
  bool CreateResources();
  bool CreatePipelines();
  void DestroyScreenResources();
  Vec3 SnapOrigin(const Vec3& camera, u32 cascade) const;

  Device& device_;
  Settings settings_;
  SamplerHandle sampler_;        // nearest clamp (atlases / depth)
  SamplerHandle linear_sampler_; // linear clamp (screen-cache colour)
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
  PipelineHandle gather_pipeline_;
  PipelineHandle denoise_pipeline_;
  PipelineHandle upscale_pipeline_;
  PipelineHandle history_pipeline_;

  // M2 screen-side persistent images (render resolution). Screen-cache history is
  // written late (AddHistoryCopy) and read early next frame (gather); the
  // irradiance temporal history ping-pongs by frame parity.
  static constexpr u32 kGatherDivisor = 2;  // half res (2), quarter-res ready (4)
  Extent2D screen_extent_{};
  GpuImage screen_color_hist_;               // rgba16f lit HDR snapshot
  GpuImage screen_depth_hist_;               // r32f depth snapshot
  GpuImage irr_hist_[2];                     // rgba16f temporal irradiance history
  ResourceState screen_color_state_ = ResourceState::kUndefined;
  ResourceState screen_depth_state_ = ResourceState::kUndefined;
  ResourceState irr_hist_state_[2] = {ResourceState::kUndefined, ResourceState::kUndefined};
  // Handles imported this frame in AddGatherChain, reused by AddHistoryCopy.
  ResourceHandle screen_color_handle_{};
  ResourceHandle screen_depth_handle_{};

  bool atlas_initialized_ = false;
  bool history_valid_ = false;
  bool screen_history_valid_ = false;
  bool screen_reset_ = true;  // force a temporal reset after (re)creating the images
  bool clear_hash_ = true;  // first update zeroes the hash
  // Per-cascade "has been blended at least once since creation/teleport". Only
  // the current (frame % 4) cascade blends each frame, so first-blend reset and
  // sample validity are tracked per cascade, not as one global flag.
  bool cascade_valid_[kCascades] = {};
  Vec3 blended_origin_[kCascades] = {};
  Vec3 last_camera_{};
};

}  // namespace rx::render

#endif  // RX_RENDER_RCGI_H_
