#ifndef RX_RENDER_RCGI_H_
#define RX_RENDER_RCGI_H_

#include <memory>
#include <span>

#include "core/math.h"
#include "render/core/bindless.h"
#include "render/core/render_graph.h"
#include "render/gi/light_grid.h"
#include "render/gi/rcgi_history.h"
#include "render/gi/rcgi_interior.h"
#include "render/rhi/device.h"

namespace rx::render {

class RayTracingContext;
class SdfClipmap;

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

  // Per-frame leak/occlusion-hardening state (Phase 3), uploaded into the globals
  // UBO by AddToGraph and read by every RCGI pass. Each toggle has its own env in
  // the renderer (RX_RCGI_INTERIOR / RX_RCGI_RELOCATE / RX_RCGI_PROBE_AO); the
  // interior-volume count is owned by the system (SetInteriorVolumes).
  struct FrameConfig {
    bool authored_interior = false;  // raw lighting mode, independent of feature gates
    bool interior = false;           // ray misses fall back to interior_ambient, not sky
    Vec3 interior_ambient{0, 0, 0};  // authored indoor ambient
    bool relocate = true;            // per-probe backface relocation + disable
    bool classify = true;            // interior-volume cross-class probe rejection
    bool probe_ao = true;            // attenuate cascade fallback by relative hit distance
    f32 probe_ao_scale = 0.6f;
    f32 probe_ao_bias = 0.4f;
  };

  // `rt_available` selects which trace pipelines are built. When true, the
  // hardware ray-query pipelines (probe trace, cache shade, gather chain) plus
  // the software SDF variants are all created (so RX_RCGI_SW A/B works on RT
  // hardware). When false, ONLY the software variants + the shared/probes-only
  // pipelines are created -- a SPIR-V module declaring RayQuery can fail pipeline
  // creation on a non-ray-query device, so the hw pipelines are skipped entirely.
  static std::unique_ptr<RcgiSystem> Create(Device& device, TextureView sky_view,
                                            SamplerHandle sky_sampler, BindlessRegistry& bindless,
                                            bool rt_available);
  ~RcgiSystem();

  RcgiSystem(const RcgiSystem&) = delete;
  RcgiSystem& operator=(const RcgiSystem&) = delete;

  void Configure(const Settings& settings) { settings_ = settings; }

  // World-side update for the current frame's cascade (round-robin frame % 4):
  // snaps the cascade, uploads the globals UBO, and records trace/args/shade/
  // blend/border. `light_grid` must have been updated earlier in the graph;
  // `lights` is the same StructuredBuffer<Light> the cluster/light-grid consume.
  //
  // When `sdf` is non-null the world side runs in SOFTWARE mode: the probe trace
  // and cache shade sphere-trace the SDF clipmap instead of the TLAS (no ray
  // query), and `raytracing` may be null. The caller must have composed the
  // clipmap earlier in the graph this frame. When `sdf` is null the hardware
  // (ray-query) path runs and `raytracing` must be valid.
  void AddToGraph(RenderGraph& graph, RayTracingContext* raytracing, u32 tlas_slot,
                  const LightGrid& light_grid, const GpuBuffer& lights, const Vec3& camera,
                  const Vec3& sun_direction, f32 sun_intensity, const Vec3& sun_color,
                  u32 frame_index, const FrameConfig& config, bool async = false,
                  const SdfClipmap* sdf = nullptr);

  // Upload the active interior volumes (<= kMaxInteriorVolumes; extras dropped).
  // AABBs in world space; probes/samples inside a volume classify as indoor and
  // do not blend with outdoor probes (item 9b). Empty span disables classify.
  // Cheap: just repacks a small host-visible buffer, so it may be called each
  // frame from the game's forwarding path.
  void SetInteriorVolumes(std::span<const InteriorVolume> volumes, u32 frame_index);

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

  // The gather's denoised per-pixel diffuse SH (3x RGBA16F: R,G,B channel SH
  // vectors, gather resolution), valid for the CURRENT frame's graph only after
  // AddGatherChain ran (not the probes-only AddResolvePass). Consumed by the
  // specular ray-skip (reflection_trace) to replace TLAS rays on rough /
  // off-mirror pixels. Returns false when no SH was produced this frame.
  bool denoised_sh(ResourceHandle out_sh[3], Extent2D& extent) const {
    if (!denoised_sh_valid_) return false;
    out_sh[0] = denoised_sh_[0];
    out_sh[1] = denoised_sh_[1];
    out_sh[2] = denoised_sh_[2];
    extent = denoised_sh_extent_;
    return true;
  }

  // Bundle of the RCGI irradiance-cascade resources needed to call
  // SampleRcgiIrradiance from a pass outside the gather. The specular bounce in
  // reflection_trace samples DDGI's probe atlas, which is empty under RCGI
  // (black indirect at reflection hits); with this it reads the RCGI cascades
  // instead. The atlases live in kGeneral once the world side is built; the
  // globals UBO must be the one already uploaded for the current frame (the
  // reflection pass records after AddToGraph, so `frame_index % 2` is valid).
  struct IrradianceBinding {
    TextureView irradiance{};
    TextureView visibility{};
    const GpuBuffer* globals = nullptr;
    const GpuBuffer* probe_meta = nullptr;
    const GpuBuffer* interior_vols = nullptr;
    SamplerHandle sampler{};
    bool valid = false;
  };
  IrradianceBinding irradiance_binding(u32 frame_index) const {
    IrradianceBinding b;
    if (!irradiance_ || !visibility_) return b;
    b.irradiance = irradiance_.view;
    b.visibility = visibility_.view;
    b.globals = &globals_buffers_[frame_index % 2];
    b.probe_meta = &probe_meta_;
    b.interior_vols = &interior_volumes_[frame_index % 2];
    b.sampler = sampler_;
    b.valid = true;
    return b;
  }

  // Zero the hash on the next update (camera teleports / big jumps).
  void RequestReset() {
    clear_hash_ = true;
    for (u32 c = 0; c < kCascades; ++c) cascade_valid_[c] = false;
    screen_history_valid_ = false;
    screen_reset_ = true;
  }

  // Gather resolution scale (RX_RCGI_GATHER_SCALE): 2 = half res (default),
  // 4 = quarter res (opt-in; AC Shadows shipped quarter-res diffuse on consoles).
  // The denoise radius widens automatically at quarter to avoid splotching.
  void set_gather_scale(u32 scale) { gather_divisor_ = (scale >= 4u) ? 4u : 2u; }
  u32 gather_scale() const { return gather_divisor_; }

  // Material-ID denoiser mask (RX_RCGI_DENOISE_MASK, item 22a/b): reject
  // cross-class neighbours in the spatial denoise + temporal reprojection so
  // vegetation/character indirect does not bleed onto opaque surfaces. On by
  // default; the setter exposes an A/B toggle.
  void set_denoise_mask(bool on) { denoise_mask_ = on; }

 private:
  struct RcgiGlobals {
    f32 cascade_origin[kCascades][4];  // xyz origin, w probe spacing
    f32 camera_pos[4];                 // xyz eye, w LOD distance
    f32 sun_direction[4];              // xyz dir, w intensity
    f32 sun_color[4];                  // rgb, w rays per probe
    u32 counts[4];                     // probes x,y,z, irradiance texels
    u32 misc[4];                       // x current cascade, y frame, z cascades, w hash capacity
    f32 params[4];                     // x max ray dist, y hysteresis, z energy, w base cell
    u32 valid[4];                      // x post-blend mask, y cache-shade pre-blend mask
    f32 interior[4];                   // xyz interior ambient (miss fallback), w probe-AO scale
    u32 gi_flags[4];                   // x feature bits, y volume count, z asuint(probe-AO bias), w pad
  };

  explicit RcgiSystem(Device& device) : device_(device) {}
  bool CreateResources();
  bool CreatePipelines(bool rt_available);
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
  GpuBuffer probe_meta_;         // uint2 per (cascade, probe): packed reloc offset + flags
  GpuBuffer interior_volumes_[2];  // frame-parity: 2 float4 (min,max) per volume
  u32 interior_volume_count_ = 0;

  PipelineHandle probe_trace_pipeline_;
  PipelineHandle probe_trace_sw_pipeline_;   // SDF-clipmap software variant
  PipelineHandle args_pipeline_;
  PipelineHandle cache_shade_pipeline_;
  PipelineHandle cache_shade_sw_pipeline_;   // SDF-clipmap software variant
  PipelineHandle blend_pipeline_;
  PipelineHandle border_pipeline_;
  PipelineHandle probe_meta_pipeline_;  // per-probe relocation (Phase 3 item 10)
  PipelineHandle resolve_pipeline_;
  PipelineHandle gather_pipeline_;
  PipelineHandle denoise_pipeline_;
  PipelineHandle upscale_pipeline_;
  PipelineHandle history_pipeline_;

  // M2 screen-side persistent images (render resolution). Screen-cache history is
  // written late (AddHistoryCopy) and read early next frame (gather); the
  // irradiance temporal history ping-pongs by frame parity.
  static constexpr u32 kGatherDivisor = 2;  // default gather scale: half res
  u32 gather_divisor_ = kGatherDivisor;      // live scale (RX_RCGI_GATHER_SCALE: 2/4)
  bool denoise_mask_ = true;                  // material-id denoiser mask (RX_RCGI_DENOISE_MASK)
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
  // Denoised per-pixel SH triple exposed to the specular ray-skip (this frame).
  ResourceHandle denoised_sh_[3]{};
  Extent2D denoised_sh_extent_{};
  bool denoised_sh_valid_ = false;

  bool rt_pipelines_ = false;  // hardware ray-query pipelines were created
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
  // Effective authored interior lighting last seen by AddToGraph. Raw mode is
  // tracked separately from the RX_RCGI_INTERIOR shader gate so toggles and
  // cell-to-cell authored-light changes invalidate every retained history.
  RcgiHistoryLighting last_history_lighting_{};
  bool have_history_lighting_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_RCGI_H_
