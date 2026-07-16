#include "render/gi/rcgi.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/gi/sdf_clipmap.h"
#include "render/rhi/bindings.h"
#include "shaders/rcgi_args_cs_hlsl.h"
#include "shaders/rcgi_blend_cs_hlsl.h"
#include "shaders/rcgi_border_cs_hlsl.h"
#include "shaders/rcgi_cache_shade_cs_hlsl.h"
#include "shaders/rcgi_cache_shade_sw_cs_hlsl.h"
#include "shaders/rcgi_denoise_cs_hlsl.h"
#include "shaders/rcgi_gather_cs_hlsl.h"
#include "shaders/rcgi_history_cs_hlsl.h"
#include "shaders/rcgi_probe_meta_cs_hlsl.h"
#include "shaders/rcgi_probe_trace_cs_hlsl.h"
#include "shaders/rcgi_probe_trace_sw_cs_hlsl.h"
#include "shaders/rcgi_resolve_cs_hlsl.h"
#include "shaders/rcgi_upscale_cs_hlsl.h"

namespace rx::render {
namespace {

constexpr u32 kProbeCount =
    RcgiSystem::kProbesPerAxis * RcgiSystem::kProbesPerAxis * RcgiSystem::kProbesPerAxis;
// Per-probe relocation metadata (item 10): uint2 per (cascade, probe).
constexpr u32 kProbeMetaCount = kProbeCount * RcgiSystem::kCascades;
// Interior-volume UBO/SSBO: two float4 (min, max) per volume (item 9b).
constexpr u32 kInteriorVolFloat4s = kMaxInteriorVolumes * 2;
constexpr u32 kAtlasStride = RcgiSystem::kIrradianceTexels + 2;  // == visibility stride
constexpr u32 kAtlasWidth =
    kAtlasStride * RcgiSystem::kProbesPerAxis * RcgiSystem::kProbesPerAxis;
constexpr u32 kSlabHeight = kAtlasStride * RcgiSystem::kProbesPerAxis;
constexpr u32 kAtlasHeight = kSlabHeight * RcgiSystem::kCascades;

struct RotationPush {
  f32 rotation[12];  // three float4 rows
};
struct BlendPush {
  f32 rotation[12];
  u32 mode;
  u32 reset;
  u32 pad[2];
};
struct BorderPush {
  u32 texels;
  u32 probes_x;
  u32 probes_y;
  u32 y_base;
};
struct MetaPush {
  f32 rotation[12];  // three float4 rows (same as the trace, to recompute dirs)
  u32 reset;
  u32 pad[3];
};
struct ResolvePush {
  f32 inv_view_proj[16];
  f32 inv_size[2];
  f32 pad[2];
  f32 camera_pos[4];
};
struct GatherPush {
  f32 inv_view_proj[16];
  f32 prev_view_proj[16];
  f32 camera_pos[4];  // xyz eye, w near plane
  u32 dims[4];        // full_w, full_h, gather_w, gather_h
  u32 misc[4];        // frame_index, screen_valid, asuint(ray_max), pad
};
struct DenoisePush {
  u32 dims[4];    // full_w, full_h, gather_w, gather_h
  u32 misc[4];    // dir_x, dir_y, radius, pad
  f32 params[4];  // near_plane, ...
};
struct UpscalePush {
  u32 dims[4];    // full_w, full_h, gather_w, gather_h
  f32 params[4];  // near_plane, intensity, max_history, reset
  u32 misc[4];    // frame_index, pad...
};
struct HistoryPush {
  u32 size[2];
  u32 pad[2];
};

// Uniformly random rotation per frame (axis-angle from a weyl hash), same scheme
// as DdgiSystem so the fibonacci sphere covers all directions over time.
void FrameRotation(u32 frame_index, f32 out_rows[12]) {
  auto hash = [](u32 v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v;
  };
  f32 u1 = static_cast<f32>(hash(frame_index) & 0xffffff) / 16777215.0f;
  f32 u2 = static_cast<f32>(hash(frame_index + 1) & 0xffffff) / 16777215.0f;
  f32 u3 = static_cast<f32>(hash(frame_index + 2) & 0xffffff) / 16777215.0f;
  f32 angle = u1 * 6.2831853f;
  f32 z = u2 * 2.0f - 1.0f;
  f32 r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  f32 phi = u3 * 6.2831853f;
  Vec3 axis{r * std::cos(phi), r * std::sin(phi), z};
  f32 c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
  f32 rows[12] = {
      t * axis.x * axis.x + c,          t * axis.x * axis.y - s * axis.z,
      t * axis.x * axis.z + s * axis.y, 0,
      t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,
      t * axis.y * axis.z - s * axis.x, 0,
      t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x,
      t * axis.z * axis.z + c,          0,
  };
  std::memcpy(out_rows, rows, sizeof(rows));
}

}  // namespace

std::unique_ptr<RcgiSystem> RcgiSystem::Create(Device& device, TextureView sky_view,
                                               SamplerHandle sky_sampler,
                                               BindlessRegistry& bindless, bool rt_available) {
  auto rcgi = std::unique_ptr<RcgiSystem>(new RcgiSystem(device));
  rcgi->sky_view_ = sky_view;
  rcgi->sky_sampler_ = sky_sampler;
  rcgi->bindless_ = &bindless;
  if (!rcgi->CreateResources() || !rcgi->CreatePipelines(rt_available)) return nullptr;
  return rcgi;
}

Vec3 RcgiSystem::SnapOrigin(const Vec3& camera, u32 cascade) const {
  f32 spacing = kBaseSpacing * static_cast<f32>(1u << cascade);
  f32 half = (kProbesPerAxis - 1) * spacing * 0.5f;
  return Vec3{std::floor((camera.x - half) / spacing) * spacing,
             std::floor((camera.y - half) / spacing) * spacing,
             std::floor((camera.z - half) / spacing) * spacing};
}

bool RcgiSystem::CreateResources() {
  sampler_ = device_.GetSampler({.mip_filter = Filter::kNearest,
                                 .address_u = AddressMode::kClampToEdge,
                                 .address_v = AddressMode::kClampToEdge,
                                 .address_w = AddressMode::kClampToEdge,
                                 .max_lod = 0.0f});
  linear_sampler_ = device_.GetSampler({.min_filter = Filter::kLinear,
                                        .mag_filter = Filter::kLinear,
                                        .mip_filter = Filter::kNearest,
                                        .address_u = AddressMode::kClampToEdge,
                                        .address_v = AddressMode::kClampToEdge,
                                        .address_w = AddressMode::kClampToEdge,
                                        .max_lod = 0.0f});
  if (!sampler_ || !linear_sampler_) return false;

  // TransferDst on the atlases so they can be cleared to black at creation: a
  // cascade blends incrementally (one per frame) and cascades not yet blended
  // this session must read as 0, never as undefined image memory.
  TextureUsageFlags atlas_usage =
      kTextureUsageSampled | kTextureUsageStorage | kTextureUsageTransferDst;
  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageStorage;
  irradiance_ =
      device_.CreateImage2D(Format::kRGBA16Float, {kAtlasWidth, kAtlasHeight}, atlas_usage);
  visibility_ =
      device_.CreateImage2D(Format::kRGBA16Float, {kAtlasWidth, kAtlasHeight}, atlas_usage);
  rays_ = device_.CreateImage2D(Format::kRGBA16Float, {kRaysPerProbe, kProbeCount}, usage);
  if (!irradiance_ || !visibility_ || !rays_) return false;

  // Clear the atlases to black now (belt and braces alongside the per-cascade
  // valid mask), then settle all three in GENERAL where the passes keep them.
  // rays_ is fully rewritten each frame before it is read, so it only needs the
  // undefined->general settle.
  device_.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier to_clear[2] = {
        Transition(irradiance_, ResourceState::kUndefined, ResourceState::kCopyDst),
        Transition(visibility_, ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    const f32 black[4] = {0, 0, 0, 0};
    cmd.ClearColor(irradiance_, black);
    cmd.ClearColor(visibility_, black);
    TextureBarrier to_general[3] = {
        Transition(irradiance_, ResourceState::kCopyDst, ResourceState::kGeneral),
        Transition(visibility_, ResourceState::kCopyDst, ResourceState::kGeneral),
        Transition(rays_, ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  atlas_initialized_ = true;  // the first-touch transition in AddToGraph is done

  // state_ / active_meta_ are FillBuffer-cleared, so they need TRANSFER_DST.
  state_ = device_.CreateBuffer(static_cast<u64>(kHashCapacity) * kEntryStride * sizeof(u32),
                                kBufferUsageStorage | kBufferUsageTransferDst);
  radiance_ = device_.CreateBuffer(static_cast<u64>(kHashCapacity) * 2 * sizeof(u32),
                                   kBufferUsageStorage);
  active_list_ = device_.CreateBuffer(static_cast<u64>(kActiveCapacity) * sizeof(u32),
                                      kBufferUsageStorage);
  active_meta_ = device_.CreateBuffer(4 * sizeof(u32), kBufferUsageStorage | kBufferUsageTransferDst);
  dispatch_args_ =
      device_.CreateBuffer(4 * sizeof(u32), kBufferUsageStorage | kBufferUsageIndirect);
  if (!state_ || !radiance_ || !active_list_ || !active_meta_ || !dispatch_args_) return false;

  // Per-probe relocation metadata. FillBuffer-cleared on reset, so needs
  // TRANSFER_DST; zeroed now so an unrelocated read is a no-op (offset 0, not
  // disabled) even before the meta pass first runs.
  probe_meta_ = device_.CreateBuffer(static_cast<u64>(kProbeMetaCount) * 2 * sizeof(u32),
                                     kBufferUsageStorage | kBufferUsageTransferDst);
  // Interior volumes: host-visible, updated by SetInteriorVolumes. Zeroed so a
  // stale slot never classifies as inside before the game forwards volumes.
  interior_volumes_ = device_.CreateBuffer(
      static_cast<u64>(kInteriorVolFloat4s) * 4 * sizeof(f32), kBufferUsageStorage, true);
  if (!probe_meta_ || !interior_volumes_ || !interior_volumes_.mapped) return false;
  std::memset(interior_volumes_.mapped, 0, static_cast<size_t>(kInteriorVolFloat4s) * 4 * sizeof(f32));
  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.FillBuffer(probe_meta_, 0, probe_meta_.size, 0);
  });

  for (GpuBuffer& b : globals_buffers_) {
    b = device_.CreateBuffer(sizeof(RcgiGlobals), kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }
  return true;
}

bool RcgiSystem::CreatePipelines(bool rt_available) {
  rt_pipelines_ = rt_available;
  // Software SDF-clipmap descriptor set (set 1) shared by both sw variants:
  // {0 sdf globals UBO, 1..3 distance/albedo/emissive Texture3D, 4 sampler}.
  const PipelineBindings kSdfSet{.slots = {{0, BindingType::kUniformBuffer},
                                           {1, BindingType::kSampledImage},
                                           {2, BindingType::kSampledImage},
                                           {3, BindingType::kSampledImage},
                                           {4, BindingType::kSampler}}};
  // Software probe trace: hardware set 0 minus the accel-struct slot (1), plus
  // the SDF set. Contains no RayQuery, so it creates on non-ray-query devices.
  probe_trace_sw_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_probe_trace_sw_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {2, BindingType::kUniformBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kStorageBuffer},
                          {6, BindingType::kCombinedTextureSampler},
                          {7, BindingType::kStorageBuffer}}},
               kSdfSet},
      .push_constant_size = sizeof(RotationPush),
      .debug_name = "rcgi_probe_trace_sw",
  });
  // Software cache shade: hardware set 0 minus the accel-struct slot (5), plus
  // the SDF set. No bindless material tables (colour comes from the entry).
  cache_shade_sw_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_cache_shade_sw_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kUniformBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {6, BindingType::kStorageBuffer},
                          {7, BindingType::kStorageBuffer},
                          {8, BindingType::kStorageBuffer},
                          {9, BindingType::kUniformBuffer},
                          {10, BindingType::kCombinedTextureSampler},
                          {11, BindingType::kCombinedTextureSampler},
                          {12, BindingType::kStorageBuffer},
                          {13, BindingType::kStorageBuffer}}},
               kSdfSet},
      .push_constant_size = 0,
      .debug_name = "rcgi_cache_shade_sw",
  });
  if (!probe_trace_sw_pipeline_ || !cache_shade_sw_pipeline_) {
    RX_ERROR("rcgi software pipeline creation failed");
    return false;
  }

  if (rt_available) {
  probe_trace_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_probe_trace_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kAccelStruct},
                          {2, BindingType::kUniformBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kStorageBuffer},
                          {6, BindingType::kCombinedTextureSampler},
                          {7, BindingType::kStorageBuffer}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = sizeof(RotationPush),
      .debug_name = "rcgi_probe_trace",
  });
  }
  args_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_args_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer}}}},
      .push_constant_size = 0,
      .debug_name = "rcgi_args",
  });
  if (rt_available) {
  cache_shade_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_cache_shade_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kUniformBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kAccelStruct},
                          {6, BindingType::kStorageBuffer},
                          {7, BindingType::kStorageBuffer},
                          {8, BindingType::kStorageBuffer},
                          {9, BindingType::kUniformBuffer},
                          {10, BindingType::kCombinedTextureSampler},
                          {11, BindingType::kCombinedTextureSampler},
                          {12, BindingType::kStorageBuffer},
                          {13, BindingType::kStorageBuffer}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = 0,
      .debug_name = "rcgi_cache_shade",
  });
  }
  blend_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_blend_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kUniformBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kStorageBuffer},
                          {6, BindingType::kCombinedTextureSampler}}}},  // sky (cache-miss fallback)
      .push_constant_size = sizeof(BlendPush),
      .debug_name = "rcgi_blend",
  });
  // Per-probe relocation (item 10): reads this frame's rays, writes probe_meta.
  probe_meta_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_probe_meta_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kUniformBuffer},
                          {2, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(MetaPush),
      .debug_name = "rcgi_probe_meta",
  });
  border_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_border_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(BorderPush),
      .debug_name = "rcgi_border",
  });
  resolve_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_resolve_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kUniformBuffer},
                          {4, BindingType::kCombinedTextureSampler},
                          {5, BindingType::kCombinedTextureSampler},
                          {6, BindingType::kStorageBuffer},
                          {7, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(ResolvePush),
      .debug_name = "rcgi_resolve",
  });
  // M2 gather chain (ray-query gather + its denoise/upscale/history filters):
  // only used by the hardware resolve path, so skip on non-ray-query devices
  // (software mode forces the probes-only resolve below).
  if (rt_available) {
  gather_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_gather_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kStorageImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kAccelStruct},
                          {7, BindingType::kUniformBuffer},
                          {8, BindingType::kStorageBuffer},
                          {9, BindingType::kStorageBuffer},
                          {10, BindingType::kCombinedTextureSampler},
                          {11, BindingType::kCombinedTextureSampler},
                          {12, BindingType::kCombinedTextureSampler},
                          {13, BindingType::kCombinedTextureSampler},
                          {14, BindingType::kCombinedTextureSampler},
                          {15, BindingType::kStorageBuffer},
                          {16, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(GatherPush),
      .debug_name = "rcgi_gather",
  });
  denoise_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_denoise_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(DenoisePush),
      .debug_name = "rcgi_denoise",
  });
  upscale_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_upscale_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kSampledImage},
                          {6, BindingType::kSampledImage},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(UpscalePush),
      .debug_name = "rcgi_upscale",
  });
  history_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_history_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(HistoryPush),
      .debug_name = "rcgi_history",
  });
  }  // rt_available (gather chain)
  bool shared_ok = args_pipeline_ && blend_pipeline_ && border_pipeline_ && resolve_pipeline_ &&
                   probe_meta_pipeline_;
  bool hw_ok = !rt_available || (probe_trace_pipeline_ && cache_shade_pipeline_ &&
                                 gather_pipeline_ && denoise_pipeline_ && upscale_pipeline_ &&
                                 history_pipeline_);
  if (!shared_ok || !hw_ok) {
    RX_ERROR("rcgi pipeline creation failed");
    return false;
  }
  return true;
}

void RcgiSystem::DestroyScreenResources() {
  for (GpuImage* img : {&screen_color_hist_, &screen_depth_hist_, &irr_hist_[0], &irr_hist_[1]}) {
    if (*img) device_.DestroyImage(*img);
    *img = {};
  }
  screen_color_state_ = ResourceState::kUndefined;
  screen_depth_state_ = ResourceState::kUndefined;
  irr_hist_state_[0] = ResourceState::kUndefined;
  irr_hist_state_[1] = ResourceState::kUndefined;
  screen_extent_ = {};
  screen_history_valid_ = false;
}

bool RcgiSystem::EnsureScreenResources(Extent2D extent) {
  if (extent.width == screen_extent_.width && extent.height == screen_extent_.height &&
      screen_color_hist_) {
    return true;
  }
  DestroyScreenResources();
  screen_extent_ = extent;
  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageStorage;
  screen_color_hist_ = device_.CreateImage2D(Format::kRGBA16Float, extent, usage);
  screen_depth_hist_ = device_.CreateImage2D(Format::kR32Float, extent, usage);
  irr_hist_[0] = device_.CreateImage2D(Format::kRGBA16Float, extent, usage);
  irr_hist_[1] = device_.CreateImage2D(Format::kRGBA16Float, extent, usage);
  if (!screen_color_hist_ || !screen_depth_hist_ || !irr_hist_[0] || !irr_hist_[1]) {
    RX_ERROR("rcgi screen resource creation failed");
    // Tear down whatever was created and clear the cached extent so a later
    // frame retries (the extent fast-path above would otherwise wedge on a
    // partial success). The caller must skip the gather chain this frame.
    DestroyScreenResources();
    return false;
  }
  // Prime to kGeneral so the first frame's imports have a defined source state.
  device_.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier b[4] = {
        Transition(screen_color_hist_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(screen_depth_hist_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(irr_hist_[0], ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(irr_hist_[1], ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(b);
  });
  screen_color_state_ = ResourceState::kGeneral;
  screen_depth_state_ = ResourceState::kGeneral;
  irr_hist_state_[0] = ResourceState::kGeneral;
  irr_hist_state_[1] = ResourceState::kGeneral;
  screen_history_valid_ = false;
  screen_reset_ = true;  // the freshly created temporal history holds undefined data
  return true;
}

void RcgiSystem::AddToGraph(RenderGraph& graph, RayTracingContext* raytracing, u32 tlas_slot,
                            const LightGrid& light_grid, const GpuBuffer& lights,
                            const Vec3& camera, const Vec3& sun_direction, f32 sun_intensity,
                            const Vec3& sun_color, u32 frame_index, const FrameConfig& config,
                            bool async, const SdfClipmap* sdf) {
  // A camera teleport (bigger than cascade 0's extent) invalidates the whole
  // world cache; zero it before this frame's inserts.
  f32 jump = std::sqrt((camera.x - last_camera_.x) * (camera.x - last_camera_.x) +
                       (camera.y - last_camera_.y) * (camera.y - last_camera_.y) +
                       (camera.z - last_camera_.z) * (camera.z - last_camera_.z));
  if (history_valid_ && jump > kBaseSpacing * kProbesPerAxis) {
    clear_hash_ = true;
    // A teleport invalidates every cascade's blended history, not just the one
    // updated this frame: each must re-converge (reset=true) on its next turn.
    for (u32 c = 0; c < kCascades; ++c) cascade_valid_[c] = false;
  }
  last_camera_ = camera;

  // Interior transition: the sun, ambient and occlusion all change discontinuously
  // between an interior cell and the outdoors, so every RCGI history retains the
  // wrong radiance across the boundary. Invalidate the world hash cache, force
  // each cascade's probe atlas to re-converge (reset, like a cascade snap) and
  // drop the temporal screen history (via the gather's screen_reset_ path).
  if (have_interior_state_ && config.interior != last_interior_) {
    clear_hash_ = true;
    for (u32 c = 0; c < kCascades; ++c) cascade_valid_[c] = false;
    screen_history_valid_ = false;
    screen_reset_ = true;
  }
  last_interior_ = config.interior;
  have_interior_state_ = true;

  if (!history_valid_) {
    for (u32 c = 0; c < kCascades; ++c) {
      blended_origin_[c] = SnapOrigin(camera, c);
      cascade_valid_[c] = false;
    }
  }
  u32 current = frame_index % kCascades;
  Vec3 new_origin = SnapOrigin(camera, current);
  // Reset (hysteresis 0, full overwrite) when this cascade has never been blended
  // since creation/teleport, or when it just snapped to a new origin. Only the
  // *current* cascade blends this frame, so validity is tracked per cascade;
  // marking history globally valid after one cascade's reset (the old behaviour)
  // left the other three cascades blending against undefined atlas memory.
  bool reset = !cascade_valid_[current] || new_origin.x != blended_origin_[current].x ||
               new_origin.y != blended_origin_[current].y ||
               new_origin.z != blended_origin_[current].z;
  blended_origin_[current] = new_origin;
  history_valid_ = true;
  cascade_valid_[current] = true;  // blended this frame -> valid from here on

  f32 current_spacing = kBaseSpacing * static_cast<f32>(1u << current);

  RcgiGlobals g{};
  for (u32 c = 0; c < kCascades; ++c) {
    f32 spacing = kBaseSpacing * static_cast<f32>(1u << c);
    g.cascade_origin[c][0] = blended_origin_[c].x;
    g.cascade_origin[c][1] = blended_origin_[c].y;
    g.cascade_origin[c][2] = blended_origin_[c].z;
    g.cascade_origin[c][3] = spacing;
  }
  g.camera_pos[0] = camera.x;
  g.camera_pos[1] = camera.y;
  g.camera_pos[2] = camera.z;
  g.camera_pos[3] = kLodDistance;
  Vec3 sun = Normalize(sun_direction);
  g.sun_direction[0] = sun.x;
  g.sun_direction[1] = sun.y;
  g.sun_direction[2] = sun.z;
  g.sun_direction[3] = sun_intensity;
  g.sun_color[0] = sun_color.x;
  g.sun_color[1] = sun_color.y;
  g.sun_color[2] = sun_color.z;
  g.sun_color[3] = static_cast<f32>(kRaysPerProbe);
  g.counts[0] = kProbesPerAxis;
  g.counts[1] = kProbesPerAxis;
  g.counts[2] = kProbesPerAxis;
  g.counts[3] = kIrradianceTexels;
  g.misc[0] = current;
  g.misc[1] = frame_index;
  g.misc[2] = kCascades;
  g.misc[3] = kHashCapacity;
  g.params[0] = current_spacing * 6.0f;  // per-cascade max ray distance
  g.params[1] = settings_.hysteresis;
  g.params[2] = settings_.energy_scale;
  g.params[3] = kBaseCell;
  // Per-cascade validity mask consumed by the bounce/resolve/gather sampling
  // (RcgiCascadeValid): a cascade contributes only once blended at least once.
  // The current cascade is included here (it blends this frame); its pre-blend
  // reads in the same frame land on the black-cleared atlas, so 0, not garbage.
  u32 valid_mask = 0;
  for (u32 c = 0; c < kCascades; ++c)
    if (cascade_valid_[c]) valid_mask |= (1u << c);
  g.valid[0] = valid_mask;
  g.valid[1] = 0;
  g.valid[2] = 0;
  g.valid[3] = 0;
  // Phase 3 leak/occlusion hardening state. Classification is only meaningful
  // with volumes present; fold that into the flag so the shaders can early-out.
  const bool classify = config.classify && interior_volume_count_ > 0;
  u32 gi_bits = 0;
  if (config.interior) gi_bits |= 1u;   // kRcgiFlagInterior
  if (config.relocate) gi_bits |= 2u;   // kRcgiFlagRelocate
  if (config.probe_ao) gi_bits |= 4u;   // kRcgiFlagProbeAo
  if (classify) gi_bits |= 8u;          // kRcgiFlagClassify
  g.interior[0] = config.interior_ambient.x;
  g.interior[1] = config.interior_ambient.y;
  g.interior[2] = config.interior_ambient.z;
  g.interior[3] = config.probe_ao_scale;
  g.gi_flags[0] = gi_bits;
  g.gi_flags[1] = interior_volume_count_;
  std::memcpy(&g.gi_flags[2], &config.probe_ao_bias, sizeof(f32));
  g.gi_flags[3] = 0;
  std::memcpy(globals_buffers_[frame_index % 2].mapped, &g, sizeof(g));

  RotationPush trace_push{};
  FrameRotation(frame_index, trace_push.rotation);

  bool do_clear = clear_hash_;
  clear_hash_ = false;

  const bool software = sdf != nullptr;
  const bool relocate = config.relocate;
  graph.AddPass(
      "rcgi", [async](RenderGraph::PassBuilder& b) { if (async) b.Async(); },
      [this, raytracing, tlas_slot, &light_grid, &lights, trace_push, frame_index, current, reset,
       do_clear, software, sdf, relocate](PassContext& ctx) {
        const GpuBuffer& globals = globals_buffers_[frame_index % 2];
        // Software mode binds the SDF clipmap resources into set 1 for both the
        // probe trace and cache shade sw variants (mirrors sdf_debug's wiring).
        auto bind_sdf = [&](u32 set) {
          const GpuBuffer& sdf_globals = sdf->globals(frame_index);
          ctx.cmd->BindTransient(
              set, {Bind::Uniform(0, sdf_globals, 0, sdf_globals.size),
                    InGeneral(Bind::Sampled(1, sdf->distance_volume())),
                    InGeneral(Bind::Sampled(2, sdf->albedo_volume())),
                    InGeneral(Bind::Sampled(3, sdf->emissive_volume())),
                    Bind::Sampler(4, sdf->sampler())});
        };

        // First touch transitions the atlases from UNDEFINED to GENERAL.
        if (!atlas_initialized_) {
          atlas_initialized_ = true;
          TextureBarrier barriers[3] = {
              Transition(irradiance_, ResourceState::kUndefined, ResourceState::kGeneral),
              Transition(visibility_, ResourceState::kUndefined, ResourceState::kGeneral),
              Transition(rays_, ResourceState::kUndefined, ResourceState::kGeneral)};
          ctx.cmd->TextureBarriers(barriers);
        } else {
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeRead, BarrierScope::kComputeWrite);
        }

        // Zero the hash on first frame / teleport (garbage keys = false hits).
        // The probe trace consumes these via InterlockedCompareExchange, which
        // both reads and writes, so the clear must be visible to compute READS
        // as well as writes (kComputeReadWrite, not just kComputeWrite).
        if (do_clear) {
          ctx.cmd->FillBuffer(state_, 0, state_.size, 0);
          ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kComputeReadWrite);
        }
        // Per-frame: reset the active-cell counter (read+incremented by InterlockedAdd).
        ctx.cmd->FillBuffer(active_meta_, 0, active_meta_.size, 0);
        ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kComputeReadWrite);

        // Cascade (re)snapped: its cells now cover different world space, so any
        // carried relocation offset is meaningless. Zero the current cascade's
        // slice before the trace reads it (item 10). The trace reads probe_meta
        // (InterlockedCompareExchange-free, plain load), so a plain compute-read
        // hazard barrier suffices.
        const u64 meta_slice = static_cast<u64>(kProbeCount) * 2 * sizeof(u32);
        if (relocate && reset) {
          ctx.cmd->FillBuffer(probe_meta_, current * meta_slice, meta_slice, 0);
          ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kComputeRead);
        }

        // Probe trace: register hits into the cache, sky into the rays buffer.
        // Software mode sphere-traces the SDF clipmap (no TLAS, no bindless).
        if (software) {
          ctx.cmd->BindPipeline(probe_trace_sw_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Storage(0, rays_), Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                  Bind::StorageBuffer(3, state_, 0, state_.size),
                  Bind::StorageBuffer(4, active_list_, 0, active_list_.size),
                  Bind::StorageBuffer(5, active_meta_, 0, active_meta_.size),
                  Bind::Combined(6, sky_view_, sky_sampler_),
                  Bind::StorageBuffer(7, probe_meta_, 0, probe_meta_.size)});
          bind_sdf(1);
        } else {
          ctx.cmd->BindPipeline(probe_trace_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Storage(0, rays_), Bind::Accel(1, raytracing->tlas(tlas_slot)),
                  Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                  Bind::StorageBuffer(3, state_, 0, state_.size),
                  Bind::StorageBuffer(4, active_list_, 0, active_list_.size),
                  Bind::StorageBuffer(5, active_meta_, 0, active_meta_.size),
                  Bind::Combined(6, sky_view_, sky_sampler_),
                  Bind::StorageBuffer(7, probe_meta_, 0, probe_meta_.size)});
          ctx.cmd->BindSet(1, bindless_->set());
        }
        ctx.cmd->Push(trace_push);
        ctx.cmd->Dispatch((kRaysPerProbe + 31) / 32, kProbeCount, 1);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Build the indirect dispatch args for the shade pass.
        ctx.cmd->BindPipeline(args_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, active_meta_, 0, active_meta_.size),
                                   Bind::StorageBuffer(1, dispatch_args_, 0, dispatch_args_.size)});
        ctx.cmd->Dispatch(1, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kIndirectArgs);

        // Cache shade: one thread per active cell, resolve + light + store.
        // Software mode reads surface colour from the entry (packed by the sw
        // probe trace) and traces sun occlusion against the SDF clipmap.
        if (software) {
          ctx.cmd->BindPipeline(cache_shade_sw_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Uniform(0, globals, 0, sizeof(RcgiGlobals)),
                  Bind::StorageBuffer(1, state_, 0, state_.size),
                  Bind::StorageBuffer(2, radiance_, 0, radiance_.size),
                  Bind::StorageBuffer(3, active_list_, 0, active_list_.size),
                  Bind::StorageBuffer(4, active_meta_, 0, active_meta_.size),
                  Bind::StorageBuffer(6, lights, 0, lights.size),
                  Bind::StorageBuffer(7, light_grid.counts_buffer(), 0,
                                      light_grid.counts_buffer().size),
                  Bind::StorageBuffer(8, light_grid.ids_buffer(), 0, light_grid.ids_buffer().size),
                  Bind::Uniform(9, light_grid.params_buffer(frame_index), 0,
                                LightGrid::params_size()),
                  InGeneral(Bind::Combined(10, irradiance_.view, sampler_)),
                  InGeneral(Bind::Combined(11, visibility_.view, sampler_)),
                  Bind::StorageBuffer(12, probe_meta_, 0, probe_meta_.size),
                  Bind::StorageBuffer(13, interior_volumes_, 0, interior_volumes_.size)});
          bind_sdf(1);
        } else {
          ctx.cmd->BindPipeline(cache_shade_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Uniform(0, globals, 0, sizeof(RcgiGlobals)),
                  Bind::StorageBuffer(1, state_, 0, state_.size),
                  Bind::StorageBuffer(2, radiance_, 0, radiance_.size),
                  Bind::StorageBuffer(3, active_list_, 0, active_list_.size),
                  Bind::StorageBuffer(4, active_meta_, 0, active_meta_.size),
                  Bind::Accel(5, raytracing->tlas(tlas_slot)),
                  Bind::StorageBuffer(6, lights, 0, lights.size),
                  Bind::StorageBuffer(7, light_grid.counts_buffer(), 0,
                                      light_grid.counts_buffer().size),
                  Bind::StorageBuffer(8, light_grid.ids_buffer(), 0, light_grid.ids_buffer().size),
                  Bind::Uniform(9, light_grid.params_buffer(frame_index), 0,
                                LightGrid::params_size()),
                  InGeneral(Bind::Combined(10, irradiance_.view, sampler_)),
                  InGeneral(Bind::Combined(11, visibility_.view, sampler_)),
                  Bind::StorageBuffer(12, probe_meta_, 0, probe_meta_.size),
                  Bind::StorageBuffer(13, interior_volumes_, 0, interior_volumes_.size)});
          ctx.cmd->BindSet(1, bindless_->set());
        }
        ctx.cmd->DispatchIndirect(dispatch_args_, 0);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Blend rays into the current cascade's irradiance + visibility slabs.
        auto blend = [&](const GpuImage& atlas, u32 mode) {
          BlendPush push{};
          std::memcpy(push.rotation, trace_push.rotation, sizeof(push.rotation));
          push.mode = mode;
          push.reset = reset ? 1u : 0u;
          ctx.cmd->BindPipeline(blend_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, atlas),
                                     InGeneral(Bind::Combined(1, rays_.view, sampler_)),
                                     Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                                     Bind::StorageBuffer(3, state_, 0, state_.size),
                                     Bind::StorageBuffer(4, radiance_, 0, radiance_.size),
                                     Bind::StorageBuffer(5, probe_meta_, 0, probe_meta_.size),
                                     Bind::Combined(6, sky_view_, sky_sampler_)});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({kAtlasWidth, kSlabHeight});
        };
        blend(irradiance_, 0);
        blend(visibility_, 1);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Octahedral borders for the current cascade slab.
        auto border = [&](const GpuImage& atlas, u32 texels) {
          BorderPush push{texels, kProbesPerAxis * kProbesPerAxis, kProbesPerAxis,
                          current * kSlabHeight};
          ctx.cmd->BindPipeline(border_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, atlas)});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({kAtlasWidth, kSlabHeight});
        };
        border(irradiance_, kIrradianceTexels);
        border(visibility_, kVisibilityTexels);

        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Probe relocation (item 10): read back this frame's rays and refresh the
        // current cascade's per-probe offset/disable metadata, consumed by next
        // frame's trace/blend and this frame's downstream irradiance samples.
        if (relocate) {
          MetaPush mp{};
          std::memcpy(mp.rotation, trace_push.rotation, sizeof(mp.rotation));
          mp.reset = reset ? 1u : 0u;
          ctx.cmd->BindPipeline(probe_meta_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, rays_),
                                     Bind::Uniform(1, globals, 0, sizeof(RcgiGlobals)),
                                     Bind::StorageBuffer(2, probe_meta_, 0, probe_meta_.size)});
          ctx.cmd->Push(mp);
          ctx.cmd->Dispatch((kProbeCount + 63) / 64, 1, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        }
      });
}

void RcgiSystem::SetInteriorVolumes(std::span<const InteriorVolume> volumes) {
  u32 count = static_cast<u32>(std::min<size_t>(volumes.size(), kMaxInteriorVolumes));
  interior_volume_count_ = count;
  if (!interior_volumes_.mapped) return;
  // Layout: two float4 per volume (min.xyz, max.xyz); w padding unused.
  f32* dst = static_cast<f32*>(interior_volumes_.mapped);
  for (u32 i = 0; i < count; ++i) {
    dst[i * 8 + 0] = volumes[i].min.x;
    dst[i * 8 + 1] = volumes[i].min.y;
    dst[i * 8 + 2] = volumes[i].min.z;
    dst[i * 8 + 3] = 0.0f;
    dst[i * 8 + 4] = volumes[i].max.x;
    dst[i * 8 + 5] = volumes[i].max.y;
    dst[i * 8 + 6] = volumes[i].max.z;
    dst[i * 8 + 7] = 0.0f;
  }
}

ResourceHandle RcgiSystem::AddResolvePass(RenderGraph& graph, ResourceHandle depth_export,
                                          ResourceHandle normals, Extent2D extent,
                                          const Mat4& inv_view_proj, const Vec3& camera,
                                          f32 intensity, u32 frame_index) {
  denoised_sh_valid_ = false;  // probes-only path produces no SH for the ray-skip
  ResourceHandle out = graph.CreateTexture({.name = "rcgi_irradiance",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "rcgi_resolve",
      [&](RenderGraph::PassBuilder& b) {
        b.Read(depth_export, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
        b.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, depth_export, normals, out, inv_view_proj, camera, intensity,
       frame_index](PassContext& ctx) {
        const GpuBuffer& globals = globals_buffers_[frame_index % 2];
        const GpuImage& out_img = ctx.graph->image(out);
        ResolvePush push{};
        std::memcpy(push.inv_view_proj, &inv_view_proj, sizeof(push.inv_view_proj));
        push.inv_size[0] = 1.0f / static_cast<f32>(out_img.extent.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(out_img.extent.height);
        push.camera_pos[0] = camera.x;
        push.camera_pos[1] = camera.y;
        push.camera_pos[2] = camera.z;
        push.camera_pos[3] = intensity;
        ctx.cmd->BindPipeline(resolve_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, out_img), Bind::Sampled(1, ctx.graph->image(depth_export)),
                Bind::Sampled(2, ctx.graph->image(normals)),
                Bind::Uniform(3, globals, 0, sizeof(RcgiGlobals)),
                InGeneral(Bind::Combined(4, irradiance_.view, sampler_)),
                InGeneral(Bind::Combined(5, visibility_.view, sampler_)),
                Bind::StorageBuffer(6, probe_meta_, 0, probe_meta_.size),
                Bind::StorageBuffer(7, interior_volumes_, 0, interior_volumes_.size)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(out_img.extent);
      });
  return out;
}

ResourceHandle RcgiSystem::AddGatherChain(RenderGraph& graph, RayTracingContext& raytracing,
                                          u32 tlas_slot, ResourceHandle depth_export,
                                          ResourceHandle normals, ResourceHandle motion,
                                          Extent2D extent, const Mat4& inv_view_proj,
                                          const Mat4& prev_view_proj, const Vec3& camera,
                                          f32 intensity, u32 frame_index, bool reset) {
  const u32 divisor = gather_divisor_;  // 2 half (default), 4 quarter (RX_RCGI_GATHER_SCALE)
  Extent2D gather{(extent.width + divisor - 1) / divisor,
                  (extent.height + divisor - 1) / divisor};
  // Quarter-res carries ~4x fewer rays per full-res pixel, so the spatial filter
  // needs a wider footprint to avoid splotching; scale the separable radius with
  // the divisor (half=5, quarter=9). The gaussian sigma tracks the radius, so it
  // stays a proper low-pass rather than a boxier blur.
  const u32 denoise_radius = divisor >= 4u ? 9u : 5u;
  reset = reset || screen_reset_;  // freshly (re)created history must not be read
  screen_reset_ = false;
  bool screen_valid = screen_history_valid_ && !reset;

  auto tex = [&](const char* name, Format fmt, Extent2D e) {
    return graph.CreateTexture(
        {.name = name, .format = fmt, .width = e.width, .height = e.height});
  };
  // Gather-res SH triples (ping-pong: A gather/denoise-V, B denoise-H) + hitT.
  ResourceHandle a_r = tex("rcgi_sh_a_r", Format::kRGBA16Float, gather);
  ResourceHandle a_g = tex("rcgi_sh_a_g", Format::kRGBA16Float, gather);
  ResourceHandle a_b = tex("rcgi_sh_a_b", Format::kRGBA16Float, gather);
  ResourceHandle b_r = tex("rcgi_sh_b_r", Format::kRGBA16Float, gather);
  ResourceHandle b_g = tex("rcgi_sh_b_g", Format::kRGBA16Float, gather);
  ResourceHandle b_b = tex("rcgi_sh_b_b", Format::kRGBA16Float, gather);
  ResourceHandle hitt = tex("rcgi_hitt", Format::kR16Float, gather);
  ResourceHandle out = tex("rcgi_irradiance", Format::kRGBA16Float, extent);

  u32 cur = frame_index % 2;
  u32 prv = 1u - cur;
  ResourceHandle screen_color = graph.ImportImage("rcgi_screen_color", screen_color_hist_,
                                                  &screen_color_state_);
  ResourceHandle screen_depth = graph.ImportImage("rcgi_screen_depth", screen_depth_hist_,
                                                  &screen_depth_state_);
  screen_color_handle_ = screen_color;  // reused by AddHistoryCopy (this frame only)
  screen_depth_handle_ = screen_depth;
  ResourceHandle irr_cur = graph.ImportImage("rcgi_irr_hist_c", irr_hist_[cur], &irr_hist_state_[cur]);
  ResourceHandle irr_prv = graph.ImportImage("rcgi_irr_hist_p", irr_hist_[prv], &irr_hist_state_[prv]);

  const f32 near_plane = 0.1f;
  const f32 ray_max = kBaseSpacing * static_cast<f32>(kProbesPerAxis) * 2.0f;  // ~64 m reach

  // --- 1. final gather (half res) ---
  graph.AddPass(
      "rcgi_gather",
      [&](RenderGraph::PassBuilder& pb) {
        for (ResourceHandle h : {a_r, a_g, a_b, hitt}) pb.Write(h, ResourceUsage::kStorageWrite);
        pb.Read(depth_export, ResourceUsage::kSampledCompute);
        pb.Read(normals, ResourceUsage::kSampledCompute);
        pb.Read(screen_color, ResourceUsage::kSampledCompute);
        pb.Read(screen_depth, ResourceUsage::kSampledCompute);
      },
      [this, &raytracing, tlas_slot, a_r, a_g, a_b, hitt, depth_export, normals, screen_color,
       screen_depth, gather, extent, inv_view_proj, prev_view_proj, camera, frame_index,
       screen_valid, ray_max, near_plane](PassContext& ctx) {
        const GpuBuffer& globals = globals_buffers_[frame_index % 2];
        GatherPush p{};
        std::memcpy(p.inv_view_proj, &inv_view_proj, sizeof(p.inv_view_proj));
        std::memcpy(p.prev_view_proj, &prev_view_proj, sizeof(p.prev_view_proj));
        p.camera_pos[0] = camera.x; p.camera_pos[1] = camera.y; p.camera_pos[2] = camera.z;
        p.camera_pos[3] = near_plane;
        p.dims[0] = extent.width; p.dims[1] = extent.height;
        p.dims[2] = gather.width; p.dims[3] = gather.height;
        p.misc[0] = frame_index;
        p.misc[1] = screen_valid ? 1u : 0u;
        std::memcpy(&p.misc[2], &ray_max, sizeof(f32));
        ctx.cmd->BindPipeline(gather_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(a_r)), Bind::Storage(1, ctx.graph->image(a_g)),
                Bind::Storage(2, ctx.graph->image(a_b)), Bind::Storage(3, ctx.graph->image(hitt)),
                Bind::Sampled(4, ctx.graph->image(depth_export)),
                Bind::Sampled(5, ctx.graph->image(normals)),
                Bind::Accel(6, raytracing.tlas(tlas_slot)),
                Bind::Uniform(7, globals, 0, sizeof(RcgiGlobals)),
                Bind::StorageBuffer(8, state_, 0, state_.size),
                Bind::StorageBuffer(9, radiance_, 0, radiance_.size),
                InGeneral(Bind::Combined(10, irradiance_.view, sampler_)),
                InGeneral(Bind::Combined(11, visibility_.view, sampler_)),
                Bind::Combined(12, sky_view_, sky_sampler_),
                Bind::Combined(13, ctx.graph->image(screen_color).view, linear_sampler_),
                Bind::Combined(14, ctx.graph->image(screen_depth).view, sampler_),
                Bind::StorageBuffer(15, probe_meta_, 0, probe_meta_.size),
                Bind::StorageBuffer(16, interior_volumes_, 0, interior_volumes_.size)});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(gather);
      });

  // --- 2. separable bilateral denoise (H: A->B, V: B->A) ---
  auto denoise = [&](ResourceHandle in_r, ResourceHandle in_g, ResourceHandle in_b,
                     ResourceHandle out_r, ResourceHandle out_g, ResourceHandle out_b, int dx,
                     int dy) {
    graph.AddPass(
        "rcgi_denoise",
        [&](RenderGraph::PassBuilder& pb) {
          for (ResourceHandle h : {out_r, out_g, out_b}) pb.Write(h, ResourceUsage::kStorageWrite);
          for (ResourceHandle h : {in_r, in_g, in_b, hitt, depth_export, normals})
            pb.Read(h, ResourceUsage::kSampledCompute);
        },
        [this, in_r, in_g, in_b, out_r, out_g, out_b, hitt, depth_export, normals, gather, extent,
         dx, dy, near_plane, denoise_radius](PassContext& ctx) {
          DenoisePush p{};
          p.dims[0] = extent.width; p.dims[1] = extent.height;
          p.dims[2] = gather.width; p.dims[3] = gather.height;
          p.misc[0] = static_cast<u32>(dx); p.misc[1] = static_cast<u32>(dy);
          p.misc[2] = denoise_radius;
          p.misc[3] = denoise_mask_ ? 1u : 0u;  // item 22a: cross-class rejection
          p.params[0] = near_plane;
          ctx.cmd->BindPipeline(denoise_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Storage(0, ctx.graph->image(out_r)),
                  Bind::Storage(1, ctx.graph->image(out_g)),
                  Bind::Storage(2, ctx.graph->image(out_b)),
                  Bind::Sampled(3, ctx.graph->image(in_r)),
                  Bind::Sampled(4, ctx.graph->image(in_g)),
                  Bind::Sampled(5, ctx.graph->image(in_b)),
                  Bind::Sampled(6, ctx.graph->image(hitt)),
                  Bind::Sampled(7, ctx.graph->image(depth_export)),
                  Bind::Sampled(8, ctx.graph->image(normals))});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(gather);
        });
  };
  denoise(a_r, a_g, a_b, b_r, b_g, b_b, 1, 0);  // horizontal
  denoise(b_r, b_g, b_b, a_r, a_g, a_b, 0, 1);  // vertical

  // --- 3. upscale + temporal + SH resolve (full res) ---
  graph.AddPass(
      "rcgi_upscale",
      [&](RenderGraph::PassBuilder& pb) {
        pb.Write(out, ResourceUsage::kStorageWrite);
        pb.Write(irr_cur, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {a_r, a_g, a_b, depth_export, normals, motion, irr_prv})
          pb.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, out, irr_cur, a_r, a_g, a_b, depth_export, normals, motion, irr_prv, gather, extent,
       intensity, frame_index, reset, near_plane](PassContext& ctx) {
        UpscalePush p{};
        p.dims[0] = extent.width; p.dims[1] = extent.height;
        p.dims[2] = gather.width; p.dims[3] = gather.height;
        p.params[0] = near_plane;
        p.params[1] = intensity;
        p.params[2] = 48.0f;  // temporal history cap (GI is low-freq; stability > lag)
        p.params[3] = reset ? 1.0f : 0.0f;
        p.misc[0] = frame_index;
        p.misc[1] = denoise_mask_ ? 1u : 0u;  // item 22b: vegetation disocclusion handling
        ctx.cmd->BindPipeline(upscale_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out)),
                Bind::Storage(1, ctx.graph->image(irr_cur)),
                Bind::Sampled(2, ctx.graph->image(a_r)),
                Bind::Sampled(3, ctx.graph->image(a_g)),
                Bind::Sampled(4, ctx.graph->image(a_b)),
                Bind::Sampled(5, ctx.graph->image(depth_export)),
                Bind::Sampled(6, ctx.graph->image(normals)),
                Bind::Sampled(7, ctx.graph->image(motion)),
                Bind::Sampled(8, ctx.graph->image(irr_prv))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });

  // Next frame's gather may trust the screen cache once this frame fills it.
  screen_history_valid_ = true;
  // Expose the final denoised SH (the vertical pass wrote it back into A) to the
  // specular ray-skip; these transient handles are valid for this graph only.
  denoised_sh_[0] = a_r;
  denoised_sh_[1] = a_g;
  denoised_sh_[2] = a_b;
  denoised_sh_extent_ = gather;
  denoised_sh_valid_ = true;
  return out;
}

void RcgiSystem::AddHistoryCopy(RenderGraph& graph, ResourceHandle lit_color,
                                ResourceHandle depth_export, Extent2D extent) {
  if (!screen_color_handle_ || !screen_depth_handle_) return;
  ResourceHandle color_h = screen_color_handle_;
  ResourceHandle depth_h = screen_depth_handle_;
  graph.AddPass(
      "rcgi_history",
      [&](RenderGraph::PassBuilder& pb) {
        pb.Write(color_h, ResourceUsage::kStorageWrite);
        pb.Write(depth_h, ResourceUsage::kStorageWrite);
        pb.Read(lit_color, ResourceUsage::kSampledCompute);
        pb.Read(depth_export, ResourceUsage::kSampledCompute);
      },
      [this, color_h, depth_h, lit_color, depth_export, extent](PassContext& ctx) {
        HistoryPush p{};
        p.size[0] = extent.width; p.size[1] = extent.height;
        ctx.cmd->BindPipeline(history_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(color_h)),
                                   Bind::Storage(1, ctx.graph->image(depth_h)),
                                   Bind::Sampled(2, ctx.graph->image(lit_color)),
                                   Bind::Sampled(3, ctx.graph->image(depth_export))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  // Consumed: clear so a stray later call cannot double-write.
  screen_color_handle_ = {};
  screen_depth_handle_ = {};
}

RcgiSystem::~RcgiSystem() {
  DestroyScreenResources();
  for (PipelineHandle* p : {&probe_trace_pipeline_, &probe_trace_sw_pipeline_, &args_pipeline_,
                            &cache_shade_pipeline_, &cache_shade_sw_pipeline_, &blend_pipeline_,
                            &border_pipeline_, &probe_meta_pipeline_, &resolve_pipeline_,
                            &gather_pipeline_, &denoise_pipeline_, &upscale_pipeline_,
                            &history_pipeline_}) {
    if (*p) device_.DestroyPipeline(*p);
    *p = {};
  }
  device_.DestroyImage(irradiance_);
  device_.DestroyImage(visibility_);
  device_.DestroyImage(rays_);
  device_.DestroyBuffer(state_);
  device_.DestroyBuffer(radiance_);
  device_.DestroyBuffer(active_list_);
  device_.DestroyBuffer(active_meta_);
  device_.DestroyBuffer(dispatch_args_);
  device_.DestroyBuffer(probe_meta_);
  device_.DestroyBuffer(interior_volumes_);
  for (GpuBuffer& b : globals_buffers_) device_.DestroyBuffer(b);
}

}  // namespace rx::render
