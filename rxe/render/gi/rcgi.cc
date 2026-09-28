#include "rxe/render/gi/rcgi.h"

#include <math.h>
#include <string.h>

#include "base/containers/span.h"
#include "base/memory/mem_ops.h"
#include "base/memory/unique_pointer.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "rxe/render/gi/raytracing.h"
#include "rxe/render/gi/sdf_clipmap.h"
#include "rxe/gpu/rhi/bindings.h"
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
// The two matrices are the entire 128 bytes vulkan guarantees for a push block,
// so they ride in a per-frame uniform buffer and the push keeps the scalars.
struct GatherCamera {
  f32 inv_view_proj[16];
  f32 prev_view_proj[16];
};
struct GatherPush {
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
  f32 r = ::sqrtf(rx::Max(0.0f, 1.0f - z * z));
  f32 phi = u3 * 6.2831853f;
  Vec3 axis{r * ::cosf(phi), r * ::sinf(phi), z};
  f32 c = ::cosf(angle), s = ::sinf(angle), t = 1.0f - c;
  f32 rows[12] = {
      t * axis.x * axis.x + c,          t * axis.x * axis.y - s * axis.z,
      t * axis.x * axis.z + s * axis.y, 0,
      t * axis.x * axis.y + s * axis.z, t * axis.y * axis.y + c,
      t * axis.y * axis.z - s * axis.x, 0,
      t * axis.x * axis.z - s * axis.y, t * axis.y * axis.z + s * axis.x,
      t * axis.z * axis.z + c,          0,
  };
  base::MemCopy(out_rows, rows, sizeof(rows));
}

}  // namespace

base::UniquePointer<RcgiSystem> RcgiSystem::Create(gpu::Device& device, gpu::TextureView sky_view,
                                               gpu::SamplerHandle sky_sampler,
                                               BindlessRegistry& bindless, bool rt_available) {
  auto rcgi = base::UniquePointer<RcgiSystem>(new RcgiSystem(device));
  rcgi->sky_view_ = sky_view;
  rcgi->sky_sampler_ = sky_sampler;
  rcgi->bindless_ = &bindless;
  if (!rcgi->CreateResources() || !rcgi->CreatePipelines(rt_available)) return nullptr;
  return rcgi;
}

Vec3 RcgiSystem::SnapOrigin(const Vec3& camera, u32 cascade) const {
  f32 spacing = kBaseSpacing * static_cast<f32>(1u << cascade);
  f32 half = (kProbesPerAxis - 1) * spacing * 0.5f;
  return Vec3{::floorf((camera.x - half) / spacing) * spacing,
             ::floorf((camera.y - half) / spacing) * spacing,
             ::floorf((camera.z - half) / spacing) * spacing};
}

bool RcgiSystem::CreateResources() {
  sampler_ = device_.GetSampler({.mip_filter = gpu::Filter::kNearest,
                                 .address_u = gpu::AddressMode::kClampToEdge,
                                 .address_v = gpu::AddressMode::kClampToEdge,
                                 .address_w = gpu::AddressMode::kClampToEdge,
                                 .max_lod = 0.0f});
  linear_sampler_ = device_.GetSampler({.min_filter = gpu::Filter::kLinear,
                                        .mag_filter = gpu::Filter::kLinear,
                                        .mip_filter = gpu::Filter::kNearest,
                                        .address_u = gpu::AddressMode::kClampToEdge,
                                        .address_v = gpu::AddressMode::kClampToEdge,
                                        .address_w = gpu::AddressMode::kClampToEdge,
                                        .max_lod = 0.0f});
  if (!sampler_ || !linear_sampler_) return false;

  // TransferDst on the atlases so they can be cleared to black at creation: a
  // cascade blends incrementally (one per frame) and cascades not yet blended
  // this session must read as 0, never as undefined image memory.
  gpu::TextureUsageFlags atlas_usage =
      gpu::kTextureUsageSampled | gpu::kTextureUsageStorage | gpu::kTextureUsageTransferDst;
  gpu::TextureUsageFlags usage = gpu::kTextureUsageSampled | gpu::kTextureUsageStorage;
  irradiance_ =
      device_.CreateImage2D(gpu::Format::kRGBA16Float, {kAtlasWidth, kAtlasHeight}, atlas_usage);
  visibility_ =
      device_.CreateImage2D(gpu::Format::kRGBA16Float, {kAtlasWidth, kAtlasHeight}, atlas_usage);
  rays_ = device_.CreateImage2D(gpu::Format::kRGBA16Float, {kRaysPerProbe, kProbeCount}, usage);
  if (!irradiance_ || !visibility_ || !rays_) return false;

  // Clear the atlases to black now (belt and braces alongside the per-cascade
  // valid mask), then settle all three in GENERAL where the passes keep them.
  // rays_ is fully rewritten each frame before it is read, so it only needs the
  // undefined->general settle.
  device_.ImmediateSubmit([&](gpu::CommandList& cmd) {
    gpu::TextureBarrier to_clear[2] = {
        gpu::Transition(irradiance_, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst),
        gpu::Transition(visibility_, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    const f32 black[4] = {0, 0, 0, 0};
    cmd.ClearColor(irradiance_, black);
    cmd.ClearColor(visibility_, black);
    gpu::TextureBarrier to_general[3] = {
        gpu::Transition(irradiance_, gpu::ResourceState::kCopyDst, gpu::ResourceState::kGeneral),
        gpu::Transition(visibility_, gpu::ResourceState::kCopyDst, gpu::ResourceState::kGeneral),
        gpu::Transition(rays_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  atlas_initialized_ = true;  // the first-touch transition in AddToGraph is done

  // state_ / active_meta_ are FillBuffer-cleared, so they need TRANSFER_DST.
  state_ = device_.CreateBuffer(static_cast<u64>(kHashCapacity) * kEntryStride * sizeof(u32),
                                gpu::kBufferUsageStorage | gpu::kBufferUsageTransferDst);
  radiance_ = device_.CreateBuffer(static_cast<u64>(kHashCapacity) * 2 * sizeof(u32),
                                   gpu::kBufferUsageStorage);
  active_list_ = device_.CreateBuffer(static_cast<u64>(kActiveCapacity) * sizeof(u32),
                                      gpu::kBufferUsageStorage);
  active_meta_ = device_.CreateBuffer(4 * sizeof(u32), gpu::kBufferUsageStorage | gpu::kBufferUsageTransferDst);
  dispatch_args_ =
      device_.CreateBuffer(4 * sizeof(u32), gpu::kBufferUsageStorage | gpu::kBufferUsageIndirect);
  if (!state_ || !radiance_ || !active_list_ || !active_meta_ || !dispatch_args_) return false;

  // Per-probe relocation metadata. FillBuffer-cleared on reset, so needs
  // TRANSFER_DST; zeroed now so an unrelocated read is a no-op (offset 0, not
  // disabled) even before the meta pass first runs.
  probe_meta_ = device_.CreateBuffer(static_cast<u64>(kProbeMetaCount) * 2 * sizeof(u32),
                                     gpu::kBufferUsageStorage | gpu::kBufferUsageTransferDst);
  // Interior volumes: host-visible, updated by SetInteriorVolumes. Zeroed so a
  // stale slot never classifies as inside before the game forwards volumes.
  for (gpu::GpuBuffer& volumes : interior_volumes_) {
    volumes = device_.CreateBuffer(static_cast<u64>(kInteriorVolFloat4s) * 4 * sizeof(f32),
                                   gpu::kBufferUsageStorage, true);
    if (!volumes.mapped) return false;
    base::MemSet(volumes.mapped, 0,
                static_cast<size_t>(kInteriorVolFloat4s) * 4 * sizeof(f32));
  }
  if (!probe_meta_) return false;
  device_.ImmediateSubmit([&](gpu::CommandList& cmd) {
    cmd.FillBuffer(probe_meta_, 0, probe_meta_.size, 0);
  });

  for (gpu::GpuBuffer& b : globals_buffers_) {
    b = device_.CreateBuffer(sizeof(RcgiGlobals), gpu::kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }
  // One per in-flight frame: the gather rewrites it while the previous frame
  // may still be reading its own copy.
  for (gpu::GpuBuffer& b : gather_camera_) {
    b = device_.CreateBuffer(sizeof(GatherCamera), gpu::kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }
  return true;
}

bool RcgiSystem::CreatePipelines(bool rt_available) {
  rt_pipelines_ = rt_available;
  // Software SDF-clipmap descriptor set (set 1) shared by both sw variants:
  // {0 sdf globals UBO, 1..3 distance/albedo/emissive Texture3D, 4 sampler}.
  const gpu::PipelineBindings kSdfSet{.slots = {{0, gpu::BindingType::kUniformBuffer},
                                           {1, gpu::BindingType::kSampledImage},
                                           {2, gpu::BindingType::kSampledImage},
                                           {3, gpu::BindingType::kSampledImage},
                                           {4, gpu::BindingType::kSampler}}};
  // Software probe trace: hardware set 0 minus the accel-struct slot (1), plus
  // the SDF set. Contains no RayQuery, so it creates on non-ray-query devices.
  probe_trace_sw_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_probe_trace_sw_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kUniformBuffer},
                          {3, gpu::BindingType::kStorageBuffer},
                          {4, gpu::BindingType::kStorageBuffer},
                          {5, gpu::BindingType::kStorageBuffer},
                          {6, gpu::BindingType::kCombinedTextureSampler},
                          {7, gpu::BindingType::kStorageBuffer}}},
               kSdfSet},
      .push_constant_size = gpu::PushSize<RotationPush>(),
      .debug_name = "rcgi_probe_trace_sw",
  });
  // Software cache shade: hardware set 0 minus the accel-struct slot (5), plus
  // the SDF set. No bindless material tables (colour comes from the entry).
  cache_shade_sw_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_cache_shade_sw_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kUniformBuffer},
                          {1, gpu::BindingType::kStorageBuffer},
                          {2, gpu::BindingType::kStorageBuffer},
                          {3, gpu::BindingType::kStorageBuffer},
                          {4, gpu::BindingType::kStorageBuffer},
                          {6, gpu::BindingType::kStorageBuffer},
                          {7, gpu::BindingType::kStorageBuffer},
                          {8, gpu::BindingType::kStorageBuffer},
                          {9, gpu::BindingType::kUniformBuffer},
                          {10, gpu::BindingType::kCombinedTextureSampler},
                          {11, gpu::BindingType::kCombinedTextureSampler},
                          {12, gpu::BindingType::kStorageBuffer},
                          {13, gpu::BindingType::kStorageBuffer}}},
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
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kAccelStruct},
                          {2, gpu::BindingType::kUniformBuffer},
                          {3, gpu::BindingType::kStorageBuffer},
                          {4, gpu::BindingType::kStorageBuffer},
                          {5, gpu::BindingType::kStorageBuffer},
                          {6, gpu::BindingType::kCombinedTextureSampler},
                          {7, gpu::BindingType::kStorageBuffer}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = gpu::PushSize<RotationPush>(),
      .debug_name = "rcgi_probe_trace",
  });
  }
  args_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_args_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageBuffer},
                          {1, gpu::BindingType::kStorageBuffer}}}},
      .push_constant_size = 0,
      .debug_name = "rcgi_args",
  });
  if (rt_available) {
  cache_shade_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_cache_shade_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kUniformBuffer},
                          {1, gpu::BindingType::kStorageBuffer},
                          {2, gpu::BindingType::kStorageBuffer},
                          {3, gpu::BindingType::kStorageBuffer},
                          {4, gpu::BindingType::kStorageBuffer},
                          {5, gpu::BindingType::kAccelStruct},
                          {6, gpu::BindingType::kStorageBuffer},
                          {7, gpu::BindingType::kStorageBuffer},
                          {8, gpu::BindingType::kStorageBuffer},
                          {9, gpu::BindingType::kUniformBuffer},
                          {10, gpu::BindingType::kCombinedTextureSampler},
                          {11, gpu::BindingType::kCombinedTextureSampler},
                          {12, gpu::BindingType::kStorageBuffer},
                          {13, gpu::BindingType::kStorageBuffer}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = 0,
      .debug_name = "rcgi_cache_shade",
  });
  }
  blend_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_blend_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kCombinedTextureSampler},
                          {2, gpu::BindingType::kUniformBuffer},
                          {3, gpu::BindingType::kStorageBuffer},
                          {4, gpu::BindingType::kStorageBuffer},
                          {5, gpu::BindingType::kStorageBuffer},
                          {6, gpu::BindingType::kCombinedTextureSampler}}}},  // sky (cache-miss fallback)
      .push_constant_size = gpu::PushSize<BlendPush>(),
      .debug_name = "rcgi_blend",
  });
  // Per-probe relocation (item 10): reads this frame's rays, writes probe_meta.
  probe_meta_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_probe_meta_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kUniformBuffer},
                          {2, gpu::BindingType::kStorageBuffer}}}},
      .push_constant_size = gpu::PushSize<MetaPush>(),
      .debug_name = "rcgi_probe_meta",
  });
  border_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_border_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<BorderPush>(),
      .debug_name = "rcgi_border",
  });
  resolve_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_resolve_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kSampledImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kUniformBuffer},
                          {4, gpu::BindingType::kCombinedTextureSampler},
                          {5, gpu::BindingType::kCombinedTextureSampler},
                          {6, gpu::BindingType::kStorageBuffer},
                          {7, gpu::BindingType::kStorageBuffer}}}},
      .push_constant_size = gpu::PushSize<ResolvePush>(),
      .debug_name = "rcgi_resolve",
  });
  // M2 gather chain (ray-query gather + its denoise/upscale/history filters):
  // only used by the hardware resolve path, so skip on non-ray-query devices
  // (software mode forces the probes-only resolve below).
  if (rt_available) {
  gather_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_gather_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kStorageImage},
                          {3, gpu::BindingType::kStorageImage},
                          {4, gpu::BindingType::kSampledImage},
                          {5, gpu::BindingType::kSampledImage},
                          {6, gpu::BindingType::kAccelStruct},
                          {7, gpu::BindingType::kUniformBuffer},
                          {8, gpu::BindingType::kStorageBuffer},
                          {9, gpu::BindingType::kStorageBuffer},
                          {10, gpu::BindingType::kCombinedTextureSampler},
                          {11, gpu::BindingType::kCombinedTextureSampler},
                          {12, gpu::BindingType::kCombinedTextureSampler},
                          {13, gpu::BindingType::kCombinedTextureSampler},
                          {14, gpu::BindingType::kCombinedTextureSampler},
                          {15, gpu::BindingType::kStorageBuffer},
                          {16, gpu::BindingType::kStorageBuffer},
                          {17, gpu::BindingType::kUniformBuffer}}}},
      .push_constant_size = gpu::PushSize<GatherPush>(),
      .debug_name = "rcgi_gather",
  });
  denoise_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_denoise_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kStorageImage},
                          {3, gpu::BindingType::kSampledImage},
                          {4, gpu::BindingType::kSampledImage},
                          {5, gpu::BindingType::kSampledImage},
                          {6, gpu::BindingType::kSampledImage},
                          {7, gpu::BindingType::kSampledImage},
                          {8, gpu::BindingType::kSampledImage}}}},
      .push_constant_size = gpu::PushSize<DenoisePush>(),
      .debug_name = "rcgi_denoise",
  });
  upscale_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_upscale_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kSampledImage},
                          {4, gpu::BindingType::kSampledImage},
                          {5, gpu::BindingType::kSampledImage},
                          {6, gpu::BindingType::kSampledImage},
                          {7, gpu::BindingType::kSampledImage},
                          {8, gpu::BindingType::kSampledImage}}}},
      .push_constant_size = gpu::PushSize<UpscalePush>(),
      .debug_name = "rcgi_upscale",
  });
  history_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_history_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kSampledImage}}}},
      .push_constant_size = gpu::PushSize<HistoryPush>(),
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
  for (gpu::GpuImage* img : {&screen_color_hist_, &screen_depth_hist_, &irr_hist_[0], &irr_hist_[1]}) {
    if (*img) device_.DestroyImage(*img);
    *img = {};
  }
  screen_color_state_ = gpu::ResourceState::kUndefined;
  screen_depth_state_ = gpu::ResourceState::kUndefined;
  irr_hist_state_[0] = gpu::ResourceState::kUndefined;
  irr_hist_state_[1] = gpu::ResourceState::kUndefined;
  screen_extent_ = {};
  screen_history_valid_ = false;
}

bool RcgiSystem::EnsureScreenResources(gpu::Extent2D extent) {
  if (extent.width == screen_extent_.width && extent.height == screen_extent_.height &&
      screen_color_hist_) {
    return true;
  }
  DestroyScreenResources();
  screen_extent_ = extent;
  gpu::TextureUsageFlags usage = gpu::kTextureUsageSampled | gpu::kTextureUsageStorage;
  screen_color_hist_ = device_.CreateImage2D(gpu::Format::kRGBA16Float, extent, usage);
  screen_depth_hist_ = device_.CreateImage2D(gpu::Format::kR32Float, extent, usage);
  irr_hist_[0] = device_.CreateImage2D(gpu::Format::kRGBA16Float, extent, usage);
  irr_hist_[1] = device_.CreateImage2D(gpu::Format::kRGBA16Float, extent, usage);
  if (!screen_color_hist_ || !screen_depth_hist_ || !irr_hist_[0] || !irr_hist_[1]) {
    RX_ERROR("rcgi screen resource creation failed");
    // Tear down whatever was created and clear the cached extent so a later
    // frame retries (the extent fast-path above would otherwise wedge on a
    // partial success). The caller must skip the gather chain this frame.
    DestroyScreenResources();
    return false;
  }
  // Prime to kGeneral so the first frame's imports have a defined source state.
  device_.ImmediateSubmit([&](gpu::CommandList& cmd) {
    gpu::TextureBarrier b[4] = {
        gpu::Transition(screen_color_hist_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(screen_depth_hist_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(irr_hist_[0], gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(irr_hist_[1], gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral)};
    cmd.TextureBarriers(b);
  });
  screen_color_state_ = gpu::ResourceState::kGeneral;
  screen_depth_state_ = gpu::ResourceState::kGeneral;
  irr_hist_state_[0] = gpu::ResourceState::kGeneral;
  irr_hist_state_[1] = gpu::ResourceState::kGeneral;
  screen_history_valid_ = false;
  screen_reset_ = true;  // the freshly created temporal history holds undefined data
  return true;
}

void RcgiSystem::AddToGraph(RenderGraph& graph, RayTracingContext* raytracing, u32 tlas_slot,
                            const LightGrid& light_grid, const gpu::GpuBuffer& lights,
                            const Vec3& camera, const Vec3& sun_direction, f32 sun_intensity,
                            const Vec3& sun_color, u32 frame_index, const FrameConfig& config,
                            bool async, const SdfClipmap* sdf) {
  // A camera teleport (bigger than cascade 0's extent) invalidates the whole
  // world cache; zero it before this frame's inserts.
  f32 jump = ::sqrtf((camera.x - last_camera_.x) * (camera.x - last_camera_.x) +
                       (camera.y - last_camera_.y) * (camera.y - last_camera_.y) +
                       (camera.z - last_camera_.z) * (camera.z - last_camera_.z));
  if (history_valid_ && jump > kBaseSpacing * kProbesPerAxis) {
    clear_hash_ = true;
    // A teleport invalidates every cascade's blended history, not just the one
    // updated this frame: each must re-converge (reset=true) on its next turn.
    for (u32 c = 0; c < kCascades; ++c) cascade_valid_[c] = false;
  }
  last_camera_ = camera;

  Vec3 sun = Normalize(sun_direction);
  RcgiHistoryLighting history_lighting{.authored_interior = config.authored_interior,
                                       .interior_miss = config.interior,
                                       .interior_ambient = config.interior_ambient,
                                       .sun_direction = sun,
                                       .sun_intensity = sun_intensity,
                                       .sun_color = sun_color};
  // Authored interior lighting changes discontinuously at cell boundaries. The
  // raw mode remains part of the key even when RX_RCGI_INTERIOR disables the
  // miss/classification shader path.
  if (have_history_lighting_ &&
      ShouldInvalidateRcgiHistory(last_history_lighting_, history_lighting)) {
    clear_hash_ = true;
    for (u32 c = 0; c < kCascades; ++c) cascade_valid_[c] = false;
    screen_history_valid_ = false;
    screen_reset_ = true;
  }
  last_history_lighting_ = history_lighting;
  have_history_lighting_ = true;

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
  // Cache shade runs before this frame's blend. A reset cascade must therefore
  // stay out of its bounce mask until the new rays overwrite the stale slab.
  u32 previous_valid_mask = 0;
  for (u32 c = 0; c < kCascades; ++c)
    if (cascade_valid_[c]) previous_valid_mask |= (1u << c);
  RcgiCascadeValidity validity =
      ComputeRcgiCascadeValidity(previous_valid_mask, current, reset);
  g.valid[0] = validity.after_blend;
  g.valid[1] = validity.before_blend;
  g.valid[2] = 0;
  g.valid[3] = 0;
  cascade_valid_[current] = true;  // the blend recorded below primes future frames
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
  base::MemCopy(&g.gi_flags[2], &config.probe_ao_bias, sizeof(f32));
  g.gi_flags[3] = 0;
  base::MemCopy(globals_buffers_[frame_index % 2].mapped, &g, sizeof(g));

  RotationPush trace_push{};
  FrameRotation(frame_index, trace_push.rotation);

  bool do_clear = clear_hash_;
  clear_hash_ = false;

  const bool software = sdf != nullptr;
  const bool relocate = config.relocate;
  const gpu::GpuBuffer interior_volumes = interior_volumes_[frame_index % 2];
  graph.AddPass(
      "rcgi", [async](RenderGraph::PassBuilder& b) { if (async) b.Async(); },
      [this, raytracing, tlas_slot, &light_grid, &lights, trace_push, frame_index, current, reset,
       do_clear, software, sdf, relocate, interior_volumes](PassContext& ctx) {
        const gpu::GpuBuffer& globals = globals_buffers_[frame_index % 2];
        // Software mode binds the SDF clipmap resources into set 1 for both the
        // probe trace and cache shade sw variants (mirrors sdf_debug's wiring).
        auto bind_sdf = [&](u32 set) {
          const gpu::GpuBuffer& sdf_globals = sdf->globals(frame_index);
          ctx.cmd->BindTransient(
              set, {gpu::Bind::Uniform(0, sdf_globals, 0, sdf_globals.size),
                    gpu::InGeneral(gpu::Bind::Sampled(1, sdf->distance_volume())),
                    gpu::InGeneral(gpu::Bind::Sampled(2, sdf->albedo_volume())),
                    gpu::InGeneral(gpu::Bind::Sampled(3, sdf->emissive_volume())),
                    gpu::Bind::Sampler(4, sdf->sampler())});
        };

        // First touch transitions the atlases from UNDEFINED to GENERAL.
        if (!atlas_initialized_) {
          atlas_initialized_ = true;
          gpu::TextureBarrier barriers[3] = {
              gpu::Transition(irradiance_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
              gpu::Transition(visibility_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
              gpu::Transition(rays_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral)};
          ctx.cmd->TextureBarriers(barriers);
        } else {
          // Persistent world resources can be sampled by both compute gathers
          // and forward fragment shaders in the preceding frame.
          ctx.cmd->MemoryBarrier(gpu::BarrierScope::kAllCommands, gpu::BarrierScope::kComputeWrite);
        }

        // Zero the hash on first frame / teleport (garbage keys = false hits).
        // The probe trace consumes these via InterlockedCompareExchange, which
        // both reads and writes, so the clear must be visible to compute READS
        // as well as writes (kComputeReadWrite, not just kComputeWrite).
        if (do_clear) {
          ctx.cmd->FillBuffer(state_, 0, state_.size, 0);
          ctx.cmd->MemoryBarrier(gpu::BarrierScope::kTransferWrite, gpu::BarrierScope::kComputeReadWrite);
        }
        // Per-frame: reset the active-cell counter (read+incremented by InterlockedAdd).
        ctx.cmd->FillBuffer(active_meta_, 0, active_meta_.size, 0);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kTransferWrite, gpu::BarrierScope::kComputeReadWrite);

        // Cascade (re)snapped: its cells now cover different world space, so any
        // carried relocation offset is meaningless. Zero the current cascade's
        // slice before the trace reads it (item 10). The trace reads probe_meta
        // (InterlockedCompareExchange-free, plain load), so a plain compute-read
        // hazard barrier suffices.
        const u64 meta_slice = static_cast<u64>(kProbeCount) * 2 * sizeof(u32);
        if (relocate && reset) {
          ctx.cmd->FillBuffer(probe_meta_, current * meta_slice, meta_slice, 0);
          ctx.cmd->MemoryBarrier(gpu::BarrierScope::kTransferWrite, gpu::BarrierScope::kComputeRead);
        }

        // Probe trace: register hits into the cache, sky into the rays buffer.
        // Software mode sphere-traces the SDF clipmap (no TLAS, no bindless).
        if (software) {
          ctx.cmd->BindPipeline(probe_trace_sw_pipeline_);
          ctx.cmd->BindTransient(
              0, {gpu::Bind::Storage(0, rays_), gpu::Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                  gpu::Bind::StorageBuffer(3, state_, 0, state_.size),
                  gpu::Bind::StorageBuffer(4, active_list_, 0, active_list_.size),
                  gpu::Bind::StorageBuffer(5, active_meta_, 0, active_meta_.size),
                  gpu::Bind::Combined(6, sky_view_, sky_sampler_),
                  gpu::Bind::StorageBuffer(7, probe_meta_, 0, probe_meta_.size)});
          bind_sdf(1);
        } else {
          ctx.cmd->BindPipeline(probe_trace_pipeline_);
          ctx.cmd->BindTransient(
              0, {gpu::Bind::Storage(0, rays_), gpu::Bind::Accel(1, raytracing->tlas(tlas_slot)),
                  gpu::Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                  gpu::Bind::StorageBuffer(3, state_, 0, state_.size),
                  gpu::Bind::StorageBuffer(4, active_list_, 0, active_list_.size),
                  gpu::Bind::StorageBuffer(5, active_meta_, 0, active_meta_.size),
                  gpu::Bind::Combined(6, sky_view_, sky_sampler_),
                  gpu::Bind::StorageBuffer(7, probe_meta_, 0, probe_meta_.size)});
          ctx.cmd->BindSet(1, bindless_->set());
        }
        ctx.cmd->Push(trace_push);
        ctx.cmd->Dispatch((kRaysPerProbe + 31) / 32, kProbeCount, 1);

        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);

        // Build the indirect dispatch args for the shade pass.
        ctx.cmd->BindPipeline(args_pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::StorageBuffer(0, active_meta_, 0, active_meta_.size),
                                   gpu::Bind::StorageBuffer(1, dispatch_args_, 0, dispatch_args_.size)});
        ctx.cmd->Dispatch(1, 1, 1);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kIndirectArgs);

        // Cache shade: one thread per active cell, resolve + light + store.
        // Software mode reads surface colour from the entry (packed by the sw
        // probe trace) and traces sun occlusion against the SDF clipmap.
        if (software) {
          ctx.cmd->BindPipeline(cache_shade_sw_pipeline_);
          ctx.cmd->BindTransient(
              0, {gpu::Bind::Uniform(0, globals, 0, sizeof(RcgiGlobals)),
                  gpu::Bind::StorageBuffer(1, state_, 0, state_.size),
                  gpu::Bind::StorageBuffer(2, radiance_, 0, radiance_.size),
                  gpu::Bind::StorageBuffer(3, active_list_, 0, active_list_.size),
                  gpu::Bind::StorageBuffer(4, active_meta_, 0, active_meta_.size),
                  gpu::Bind::StorageBuffer(6, lights, 0, lights.size),
                  gpu::Bind::StorageBuffer(7, light_grid.counts_buffer(), 0,
                                      light_grid.counts_buffer().size),
                  gpu::Bind::StorageBuffer(8, light_grid.ids_buffer(), 0, light_grid.ids_buffer().size),
                  gpu::Bind::Uniform(9, light_grid.params_buffer(frame_index), 0,
                                LightGrid::params_size()),
                  gpu::InGeneral(gpu::Bind::Combined(10, irradiance_.view, sampler_)),
                  gpu::InGeneral(gpu::Bind::Combined(11, visibility_.view, sampler_)),
                  gpu::Bind::StorageBuffer(12, probe_meta_, 0, probe_meta_.size),
                  gpu::Bind::StorageBuffer(13, interior_volumes, 0, interior_volumes.size)});
          bind_sdf(1);
        } else {
          ctx.cmd->BindPipeline(cache_shade_pipeline_);
          ctx.cmd->BindTransient(
              0, {gpu::Bind::Uniform(0, globals, 0, sizeof(RcgiGlobals)),
                  gpu::Bind::StorageBuffer(1, state_, 0, state_.size),
                  gpu::Bind::StorageBuffer(2, radiance_, 0, radiance_.size),
                  gpu::Bind::StorageBuffer(3, active_list_, 0, active_list_.size),
                  gpu::Bind::StorageBuffer(4, active_meta_, 0, active_meta_.size),
                  gpu::Bind::Accel(5, raytracing->tlas(tlas_slot)),
                  gpu::Bind::StorageBuffer(6, lights, 0, lights.size),
                  gpu::Bind::StorageBuffer(7, light_grid.counts_buffer(), 0,
                                      light_grid.counts_buffer().size),
                  gpu::Bind::StorageBuffer(8, light_grid.ids_buffer(), 0, light_grid.ids_buffer().size),
                  gpu::Bind::Uniform(9, light_grid.params_buffer(frame_index), 0,
                                LightGrid::params_size()),
                  gpu::InGeneral(gpu::Bind::Combined(10, irradiance_.view, sampler_)),
                  gpu::InGeneral(gpu::Bind::Combined(11, visibility_.view, sampler_)),
                  gpu::Bind::StorageBuffer(12, probe_meta_, 0, probe_meta_.size),
                  gpu::Bind::StorageBuffer(13, interior_volumes, 0, interior_volumes.size)});
          ctx.cmd->BindSet(1, bindless_->set());
        }
        ctx.cmd->DispatchIndirect(dispatch_args_, 0);

        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);

        // Blend rays into the current cascade's irradiance + visibility slabs.
        auto blend = [&](const gpu::GpuImage& atlas, u32 mode) {
          BlendPush push{};
          base::MemCopy(push.rotation, trace_push.rotation, sizeof(push.rotation));
          push.mode = mode;
          push.reset = reset ? 1u : 0u;
          ctx.cmd->BindPipeline(blend_pipeline_);
          ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, atlas),
                                     gpu::InGeneral(gpu::Bind::Combined(1, rays_.view, sampler_)),
                                     gpu::Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                                     gpu::Bind::StorageBuffer(3, state_, 0, state_.size),
                                     gpu::Bind::StorageBuffer(4, radiance_, 0, radiance_.size),
                                     gpu::Bind::StorageBuffer(5, probe_meta_, 0, probe_meta_.size),
                                     gpu::Bind::Combined(6, sky_view_, sky_sampler_)});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({kAtlasWidth, kSlabHeight});
        };
        blend(irradiance_, 0);
        blend(visibility_, 1);

        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);

        // Octahedral borders for the current cascade slab.
        auto border = [&](const gpu::GpuImage& atlas, u32 texels) {
          BorderPush push{texels, kProbesPerAxis * kProbesPerAxis, kProbesPerAxis,
                          current * kSlabHeight};
          ctx.cmd->BindPipeline(border_pipeline_);
          ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, atlas)});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({kAtlasWidth, kSlabHeight});
        };
        border(irradiance_, kIrradianceTexels);
        border(visibility_, kVisibilityTexels);

        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);

        // Probe relocation (item 10): read back this frame's rays and refresh the
        // current cascade's per-probe offset/disable metadata, consumed by next
        // frame's trace/blend and this frame's downstream irradiance samples.
        if (relocate) {
          MetaPush mp{};
          base::MemCopy(mp.rotation, trace_push.rotation, sizeof(mp.rotation));
          mp.reset = reset ? 1u : 0u;
          ctx.cmd->BindPipeline(probe_meta_pipeline_);
          ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, rays_),
                                     gpu::Bind::Uniform(1, globals, 0, sizeof(RcgiGlobals)),
                                     gpu::Bind::StorageBuffer(2, probe_meta_, 0, probe_meta_.size)});
          ctx.cmd->Push(mp);
          ctx.cmd->Dispatch((kProbeCount + 63) / 64, 1, 1);
          ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);
        }
        // Inline reflection hit shading samples these persistent resources from
        // the forward fragment shader later in this frame.
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kGraphicsRead);
      });
}

void RcgiSystem::SetInteriorVolumes(base::Span<const InteriorVolume> volumes, u32 frame_index) {
  u32 count = static_cast<u32>(rx::Min<size_t>(volumes.size(), kMaxInteriorVolumes));
  interior_volume_count_ = count;
  gpu::GpuBuffer& buffer = interior_volumes_[frame_index % 2];
  if (!buffer.mapped) return;
  // Layout: two float4 per volume (min.xyz, max.xyz); w padding unused.
  f32* dst = static_cast<f32*>(buffer.mapped);
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
                                          ResourceHandle normals, gpu::Extent2D extent,
                                          const Mat4& inv_view_proj, const Vec3& camera,
                                          f32 intensity, u32 frame_index) {
  denoised_sh_valid_ = false;  // probes-only path produces no SH for the ray-skip
  ResourceHandle out = graph.CreateTexture({.name = "rcgi_irradiance",
                                            .format = gpu::Format::kRGBA16Float,
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
        const gpu::GpuBuffer& globals = globals_buffers_[frame_index % 2];
        const gpu::GpuImage& out_img = ctx.graph->image(out);
        ResolvePush push{};
        base::MemCopy(push.inv_view_proj, &inv_view_proj, sizeof(push.inv_view_proj));
        push.inv_size[0] = 1.0f / static_cast<f32>(out_img.extent.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(out_img.extent.height);
        push.camera_pos[0] = camera.x;
        push.camera_pos[1] = camera.y;
        push.camera_pos[2] = camera.z;
        push.camera_pos[3] = intensity;
        ctx.cmd->BindPipeline(resolve_pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Storage(0, out_img), gpu::Bind::Sampled(1, ctx.graph->image(depth_export)),
                gpu::Bind::Sampled(2, ctx.graph->image(normals)),
                gpu::Bind::Uniform(3, globals, 0, sizeof(RcgiGlobals)),
                gpu::InGeneral(gpu::Bind::Combined(4, irradiance_.view, sampler_)),
                gpu::InGeneral(gpu::Bind::Combined(5, visibility_.view, sampler_)),
                gpu::Bind::StorageBuffer(6, probe_meta_, 0, probe_meta_.size),
                gpu::Bind::StorageBuffer(7, interior_volumes_[frame_index % 2], 0,
                                    interior_volumes_[frame_index % 2].size)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(out_img.extent);
      });
  return out;
}

ResourceHandle RcgiSystem::AddGatherChain(RenderGraph& graph, RayTracingContext& raytracing,
                                          u32 tlas_slot, ResourceHandle depth_export,
                                          ResourceHandle normals, ResourceHandle motion,
                                          gpu::Extent2D extent, const Mat4& inv_view_proj,
                                          const Mat4& prev_view_proj, const Vec3& camera,
                                          f32 intensity, u32 frame_index, bool reset) {
  const u32 divisor = gather_divisor_;  // 2 half (default), 4 quarter (RX_RCGI_GATHER_SCALE)
  gpu::Extent2D gather{(extent.width + divisor - 1) / divisor,
                  (extent.height + divisor - 1) / divisor};
  // Quarter-res carries ~4x fewer rays per full-res pixel, so the spatial filter
  // needs a wider footprint to avoid splotching; scale the separable radius with
  // the divisor (half=5, quarter=9). The gaussian sigma tracks the radius, so it
  // stays a proper low-pass rather than a boxier blur.
  const u32 denoise_radius = divisor >= 4u ? 9u : 5u;
  reset = reset || screen_reset_;  // freshly (re)created history must not be read
  screen_reset_ = false;
  bool screen_valid = screen_history_valid_ && !reset;

  auto tex = [&](const char* name, gpu::Format fmt, gpu::Extent2D e) {
    return graph.CreateTexture(
        {.name = name, .format = fmt, .width = e.width, .height = e.height});
  };
  // Gather-res SH triples (ping-pong: A gather/denoise-V, B denoise-H) + hitT.
  ResourceHandle a_r = tex("rcgi_sh_a_r", gpu::Format::kRGBA16Float, gather);
  ResourceHandle a_g = tex("rcgi_sh_a_g", gpu::Format::kRGBA16Float, gather);
  ResourceHandle a_b = tex("rcgi_sh_a_b", gpu::Format::kRGBA16Float, gather);
  ResourceHandle b_r = tex("rcgi_sh_b_r", gpu::Format::kRGBA16Float, gather);
  ResourceHandle b_g = tex("rcgi_sh_b_g", gpu::Format::kRGBA16Float, gather);
  ResourceHandle b_b = tex("rcgi_sh_b_b", gpu::Format::kRGBA16Float, gather);
  ResourceHandle hitt = tex("rcgi_hitt", gpu::Format::kR16Float, gather);
  ResourceHandle out = tex("rcgi_irradiance", gpu::Format::kRGBA16Float, extent);

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

  // 1. final gather (half res)
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
        const gpu::GpuBuffer& globals = globals_buffers_[frame_index % 2];
        const gpu::GpuBuffer& gather_camera = gather_camera_[frame_index % 2];
        GatherCamera gc{};
        base::MemCopy(gc.inv_view_proj, &inv_view_proj, sizeof(gc.inv_view_proj));
        base::MemCopy(gc.prev_view_proj, &prev_view_proj, sizeof(gc.prev_view_proj));
        base::MemCopy(gather_camera.mapped, &gc, sizeof(gc));

        GatherPush p{};
        p.camera_pos[0] = camera.x; p.camera_pos[1] = camera.y; p.camera_pos[2] = camera.z;
        p.camera_pos[3] = near_plane;
        p.dims[0] = extent.width; p.dims[1] = extent.height;
        p.dims[2] = gather.width; p.dims[3] = gather.height;
        p.misc[0] = frame_index;
        p.misc[1] = screen_valid ? 1u : 0u;
        base::MemCopy(&p.misc[2], &ray_max, sizeof(f32));
        ctx.cmd->BindPipeline(gather_pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Storage(0, ctx.graph->image(a_r)), gpu::Bind::Storage(1, ctx.graph->image(a_g)),
                gpu::Bind::Storage(2, ctx.graph->image(a_b)), gpu::Bind::Storage(3, ctx.graph->image(hitt)),
                gpu::Bind::Sampled(4, ctx.graph->image(depth_export)),
                gpu::Bind::Sampled(5, ctx.graph->image(normals)),
                gpu::Bind::Accel(6, raytracing.tlas(tlas_slot)),
                gpu::Bind::Uniform(7, globals, 0, sizeof(RcgiGlobals)),
                gpu::Bind::StorageBuffer(8, state_, 0, state_.size),
                gpu::Bind::StorageBuffer(9, radiance_, 0, radiance_.size),
                gpu::InGeneral(gpu::Bind::Combined(10, irradiance_.view, sampler_)),
                gpu::InGeneral(gpu::Bind::Combined(11, visibility_.view, sampler_)),
                gpu::Bind::Combined(12, sky_view_, sky_sampler_),
                gpu::Bind::Combined(13, ctx.graph->image(screen_color).view, linear_sampler_),
                gpu::Bind::Combined(14, ctx.graph->image(screen_depth).view, sampler_),
                gpu::Bind::StorageBuffer(15, probe_meta_, 0, probe_meta_.size),
                gpu::Bind::StorageBuffer(16, interior_volumes_[frame_index % 2], 0,
                                    interior_volumes_[frame_index % 2].size),
                gpu::Bind::Uniform(17, gather_camera, 0, sizeof(GatherCamera))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(gather);
      });

  // 2. separable bilateral denoise (H: A->B, V: B->A)
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
              0, {gpu::Bind::Storage(0, ctx.graph->image(out_r)),
                  gpu::Bind::Storage(1, ctx.graph->image(out_g)),
                  gpu::Bind::Storage(2, ctx.graph->image(out_b)),
                  gpu::Bind::Sampled(3, ctx.graph->image(in_r)),
                  gpu::Bind::Sampled(4, ctx.graph->image(in_g)),
                  gpu::Bind::Sampled(5, ctx.graph->image(in_b)),
                  gpu::Bind::Sampled(6, ctx.graph->image(hitt)),
                  gpu::Bind::Sampled(7, ctx.graph->image(depth_export)),
                  gpu::Bind::Sampled(8, ctx.graph->image(normals))});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch2D(gather);
        });
  };
  denoise(a_r, a_g, a_b, b_r, b_g, b_b, 1, 0);  // horizontal
  denoise(b_r, b_g, b_b, a_r, a_g, a_b, 0, 1);  // vertical

  // 3. upscale + temporal + SH resolve (full res)
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
            0, {gpu::Bind::Storage(0, ctx.graph->image(out)),
                gpu::Bind::Storage(1, ctx.graph->image(irr_cur)),
                gpu::Bind::Sampled(2, ctx.graph->image(a_r)),
                gpu::Bind::Sampled(3, ctx.graph->image(a_g)),
                gpu::Bind::Sampled(4, ctx.graph->image(a_b)),
                gpu::Bind::Sampled(5, ctx.graph->image(depth_export)),
                gpu::Bind::Sampled(6, ctx.graph->image(normals)),
                gpu::Bind::Sampled(7, ctx.graph->image(motion)),
                gpu::Bind::Sampled(8, ctx.graph->image(irr_prv))});
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
                                ResourceHandle depth_export, gpu::Extent2D extent) {
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
        ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, ctx.graph->image(color_h)),
                                   gpu::Bind::Storage(1, ctx.graph->image(depth_h)),
                                   gpu::Bind::Sampled(2, ctx.graph->image(lit_color)),
                                   gpu::Bind::Sampled(3, ctx.graph->image(depth_export))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  // Consumed: clear so a stray later call cannot double-write.
  screen_color_handle_ = {};
  screen_depth_handle_ = {};
}

RcgiSystem::~RcgiSystem() {
  DestroyScreenResources();
  for (gpu::PipelineHandle* p : {&probe_trace_pipeline_, &probe_trace_sw_pipeline_, &args_pipeline_,
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
  for (gpu::GpuBuffer& volumes : interior_volumes_) device_.DestroyBuffer(volumes);
  for (gpu::GpuBuffer& b : globals_buffers_) device_.DestroyBuffer(b);
  for (gpu::GpuBuffer& b : gather_camera_) device_.DestroyBuffer(b);
}

}  // namespace rx::render
