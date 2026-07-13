#include "render/gi/rcgi.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "shaders/rcgi_args_cs_hlsl.h"
#include "shaders/rcgi_blend_cs_hlsl.h"
#include "shaders/rcgi_border_cs_hlsl.h"
#include "shaders/rcgi_cache_shade_cs_hlsl.h"
#include "shaders/rcgi_probe_trace_cs_hlsl.h"
#include "shaders/rcgi_resolve_cs_hlsl.h"

namespace rx::render {
namespace {

constexpr u32 kProbeCount =
    RcgiSystem::kProbesPerAxis * RcgiSystem::kProbesPerAxis * RcgiSystem::kProbesPerAxis;
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
struct ResolvePush {
  f32 inv_view_proj[16];
  f32 inv_size[2];
  f32 pad[2];
  f32 camera_pos[4];
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
                                               BindlessRegistry& bindless) {
  auto rcgi = std::unique_ptr<RcgiSystem>(new RcgiSystem(device));
  rcgi->sky_view_ = sky_view;
  rcgi->sky_sampler_ = sky_sampler;
  rcgi->bindless_ = &bindless;
  if (!rcgi->CreateResources() || !rcgi->CreatePipelines()) return nullptr;
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
  if (!sampler_) return false;

  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageStorage;
  irradiance_ = device_.CreateImage2D(Format::kRGBA16Float, {kAtlasWidth, kAtlasHeight}, usage);
  visibility_ = device_.CreateImage2D(Format::kRGBA16Float, {kAtlasWidth, kAtlasHeight}, usage);
  rays_ = device_.CreateImage2D(Format::kRGBA16Float, {kRaysPerProbe, kProbeCount}, usage);
  if (!irradiance_ || !visibility_ || !rays_) return false;

  state_ = device_.CreateBuffer(static_cast<u64>(kHashCapacity) * kEntryStride * sizeof(u32),
                                kBufferUsageStorage);
  radiance_ = device_.CreateBuffer(static_cast<u64>(kHashCapacity) * 2 * sizeof(u32),
                                   kBufferUsageStorage);
  active_list_ = device_.CreateBuffer(static_cast<u64>(kActiveCapacity) * sizeof(u32),
                                      kBufferUsageStorage);
  active_meta_ = device_.CreateBuffer(4 * sizeof(u32), kBufferUsageStorage);
  dispatch_args_ =
      device_.CreateBuffer(4 * sizeof(u32), kBufferUsageStorage | kBufferUsageIndirect);
  if (!state_ || !radiance_ || !active_list_ || !active_meta_ || !dispatch_args_) return false;

  for (GpuBuffer& b : globals_buffers_) {
    b = device_.CreateBuffer(sizeof(RcgiGlobals), kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }
  return true;
}

bool RcgiSystem::CreatePipelines() {
  probe_trace_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_probe_trace_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kAccelStruct},
                          {2, BindingType::kUniformBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kStorageBuffer},
                          {6, BindingType::kCombinedTextureSampler}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = sizeof(RotationPush),
      .debug_name = "rcgi_probe_trace",
  });
  args_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_args_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer}}}},
      .push_constant_size = 0,
      .debug_name = "rcgi_args",
  });
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
                          {11, BindingType::kCombinedTextureSampler}}},
               {.shared = bindless_->set_layout()}},
      .push_constant_size = 0,
      .debug_name = "rcgi_cache_shade",
  });
  blend_pipeline_ = device_.CreateComputePipeline({
      .shader = RX_SHADER(k_rcgi_blend_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kUniformBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(BlendPush),
      .debug_name = "rcgi_blend",
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
                          {5, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(ResolvePush),
      .debug_name = "rcgi_resolve",
  });
  if (!probe_trace_pipeline_ || !args_pipeline_ || !cache_shade_pipeline_ || !blend_pipeline_ ||
      !border_pipeline_ || !resolve_pipeline_) {
    RX_ERROR("rcgi pipeline creation failed");
    return false;
  }
  return true;
}

void RcgiSystem::AddToGraph(RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot,
                            const LightGrid& light_grid, const GpuBuffer& lights,
                            const Vec3& camera, const Vec3& sun_direction, f32 sun_intensity,
                            const Vec3& sun_color, u32 frame_index, bool async) {
  // A camera teleport (bigger than cascade 0's extent) invalidates the whole
  // world cache; zero it before this frame's inserts.
  f32 jump = std::sqrt((camera.x - last_camera_.x) * (camera.x - last_camera_.x) +
                       (camera.y - last_camera_.y) * (camera.y - last_camera_.y) +
                       (camera.z - last_camera_.z) * (camera.z - last_camera_.z));
  if (history_valid_ && jump > kBaseSpacing * kProbesPerAxis) clear_hash_ = true;
  last_camera_ = camera;

  if (!history_valid_) {
    for (u32 c = 0; c < kCascades; ++c) blended_origin_[c] = SnapOrigin(camera, c);
  }
  u32 current = frame_index % kCascades;
  Vec3 new_origin = SnapOrigin(camera, current);
  bool reset = !history_valid_ || new_origin.x != blended_origin_[current].x ||
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
  GpuBuffer& globals = globals_buffers_[frame_index % 2];
  std::memcpy(globals.mapped, &g, sizeof(g));

  RotationPush trace_push{};
  FrameRotation(frame_index, trace_push.rotation);

  bool do_clear = clear_hash_;
  clear_hash_ = false;

  graph.AddPass(
      "rcgi", [async](RenderGraph::PassBuilder& b) { if (async) b.Async(); },
      [this, &raytracing, tlas_slot, &light_grid, &lights, trace_push, frame_index, current, reset,
       do_clear](PassContext& ctx) {
        const GpuBuffer& globals = globals_buffers_[frame_index % 2];

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
        if (do_clear) {
          ctx.cmd->FillBuffer(state_, 0, state_.size, 0);
          ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kComputeWrite);
        }
        // Per-frame: reset the active-cell counter.
        ctx.cmd->FillBuffer(active_meta_, 0, active_meta_.size, 0);
        ctx.cmd->MemoryBarrier(BarrierScope::kTransferWrite, BarrierScope::kComputeWrite);

        // Probe trace: register hits into the cache, sky into the rays buffer.
        ctx.cmd->BindPipeline(probe_trace_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, rays_), Bind::Accel(1, raytracing.tlas(tlas_slot)),
                Bind::Uniform(2, globals, 0, sizeof(RcgiGlobals)),
                Bind::StorageBuffer(3, state_, 0, state_.size),
                Bind::StorageBuffer(4, active_list_, 0, active_list_.size),
                Bind::StorageBuffer(5, active_meta_, 0, active_meta_.size),
                Bind::Combined(6, sky_view_, sky_sampler_)});
        ctx.cmd->BindSet(1, bindless_->set());
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
        ctx.cmd->BindPipeline(cache_shade_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Uniform(0, globals, 0, sizeof(RcgiGlobals)),
                Bind::StorageBuffer(1, state_, 0, state_.size),
                Bind::StorageBuffer(2, radiance_, 0, radiance_.size),
                Bind::StorageBuffer(3, active_list_, 0, active_list_.size),
                Bind::StorageBuffer(4, active_meta_, 0, active_meta_.size),
                Bind::Accel(5, raytracing.tlas(tlas_slot)),
                Bind::StorageBuffer(6, lights, 0, lights.size),
                Bind::StorageBuffer(7, light_grid.counts_buffer(), 0,
                                    light_grid.counts_buffer().size),
                Bind::StorageBuffer(8, light_grid.ids_buffer(), 0, light_grid.ids_buffer().size),
                Bind::Uniform(9, light_grid.params_buffer(frame_index), 0, LightGrid::params_size()),
                InGeneral(Bind::Combined(10, irradiance_.view, sampler_)),
                InGeneral(Bind::Combined(11, visibility_.view, sampler_))});
        ctx.cmd->BindSet(1, bindless_->set());
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
                                     Bind::StorageBuffer(4, radiance_, 0, radiance_.size)});
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
      });
}

ResourceHandle RcgiSystem::AddResolvePass(RenderGraph& graph, ResourceHandle depth_export,
                                          ResourceHandle normals, Extent2D extent,
                                          const Mat4& inv_view_proj, const Vec3& camera,
                                          f32 intensity, u32 frame_index) {
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
                InGeneral(Bind::Combined(5, visibility_.view, sampler_))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(out_img.extent);
      });
  return out;
}

RcgiSystem::~RcgiSystem() {
  for (PipelineHandle* p : {&probe_trace_pipeline_, &args_pipeline_, &cache_shade_pipeline_,
                            &blend_pipeline_, &border_pipeline_, &resolve_pipeline_}) {
    device_.DestroyPipeline(*p);
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
  for (GpuBuffer& b : globals_buffers_) device_.DestroyBuffer(b);
}

}  // namespace rx::render
