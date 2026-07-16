#include "render/atmosphere/froxel_fog.h"

#include <cstring>

#include "core/log.h"
#include "shaders/froxel_apply_cs_hlsl.h"
#include "shaders/froxel_integrate_cs_hlsl.h"
#include "shaders/froxel_scatter_cs_hlsl.h"

namespace rx::render {
namespace {

struct ScatterPush {
  Mat4 inv_view_proj;
  Mat4 prev_view_proj;
  f32 camera_pos[4];
  f32 sun_dir_g[4];
  f32 sun_color[4];
  f32 density_params[4];
  f32 volume_params[4];
  f32 cluster_params[4];
  f32 screen_size[4];
};
struct IntegratePush {
  f32 near_plane;
  f32 far_plane;
  u32 slices;
  f32 pad;
};
struct ApplyPush {
  f32 near_plane;
  f32 far_plane;
  u32 size[2];
};

}  // namespace

bool FroxelFog::Initialize(Device& device) {
  scatter_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_froxel_scatter_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kStorageBuffer},
                          {6, BindingType::kCombinedTextureSampler},
                          {7, BindingType::kUniformBuffer},
                          {8, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(ScatterPush),
      .debug_name = "froxel_scatter",
  });
  integrate_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_froxel_integrate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(IntegratePush),
      .debug_name = "froxel_integrate",
  });
  apply_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_froxel_apply_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(ApplyPush),
      .debug_name = "froxel_apply",
  });
  if (!scatter_pipeline_ || !integrate_pipeline_ || !apply_pipeline_) {
    RX_ERROR("froxel fog pipeline creation failed");
    return false;
  }

  const TextureUsageFlags usage =
      kTextureUsageStorage | kTextureUsageSampled | kTextureUsageTransferDst;
  for (GpuImage& volume : scatter_) {
    volume = device.CreateImage3D(Format::kRGBA16Float, kSizeX, kSizeY, kSizeZ, usage);
  }
  integrated_ = device.CreateImage3D(Format::kRGBA16Float, kSizeX, kSizeY, kSizeZ, usage);
  if (!scatter_[0] || !scatter_[1] || !integrated_) {
    RX_WARN("froxel fog volumes unavailable (no 3d image support)");
    Destroy(device);
    return false;
  }

  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});
  dummy_uniform_ = device.CreateBuffer(512, kBufferUsageUniform, true);
  if (!dummy_uniform_.mapped) return false;
  std::memset(dummy_uniform_.mapped, 0, 512);

  device.ImmediateSubmit([this](CommandList& cmd) {
    // Clear the ping-pong volumes in the copy state, then settle everything in
    // GENERAL where the passes keep them.
    TextureBarrier to_clear[2] = {
        Transition(scatter_[0], ResourceState::kUndefined, ResourceState::kCopyDst),
        Transition(scatter_[1], ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    const f32 zero[4] = {0, 0, 0, 0};
    cmd.ClearColor(scatter_[0], zero);
    cmd.ClearColor(scatter_[1], zero);
    TextureBarrier to_general[3] = {
        Transition(scatter_[0], ResourceState::kCopyDst, ResourceState::kGeneral),
        Transition(scatter_[1], ResourceState::kCopyDst, ResourceState::kGeneral),
        Transition(integrated_, ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  volumes_initialized_ = true;
  return true;
}

void FroxelFog::Destroy(Device& device) {
  for (PipelineHandle* p : {&scatter_pipeline_, &integrate_pipeline_, &apply_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage& volume : scatter_) {
    if (volume) device.DestroyImage(volume);
    volume = {};
  }
  if (integrated_) device.DestroyImage(integrated_);
  integrated_ = {};
  if (dummy_uniform_) device.DestroyBuffer(dummy_uniform_);
}

void FroxelFog::AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle depth_export,
                           ResourceHandle cascade_atlas_handle, Extent2D extent,
                           const Frame& frame) {
  const u32 slot = frame.frame_index % 2;

  graph.AddPass(
      "froxel_scatter",
      [&](RenderGraph::PassBuilder& b) {
        if (cascade_atlas_handle != kInvalidResource)
          b.Read(cascade_atlas_handle, ResourceUsage::kSampledCompute);
      },
      [this, slot, cascade_atlas_handle, frame](PassContext& ctx) {
        ScatterPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.prev_view_proj = frame.prev_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = static_cast<f32>(frame.frame_index);
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_dir_g[0] = sun.x;
        push.sun_dir_g[1] = sun.y;
        push.sun_dir_g[2] = sun.z;
        push.sun_dir_g[3] = frame.anisotropy;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.sun_color[3] = frame.ambient;
        push.density_params[0] = frame.density;
        push.density_params[1] = frame.height_falloff;
        push.density_params[2] = frame.base_height;
        push.density_params[3] = 0.9f;  // temporal alpha
        push.volume_params[0] = kNear;
        push.volume_params[1] = kFar;
        push.volume_params[2] = static_cast<f32>(kSizeZ);
        push.volume_params[3] = frame.csm_active ? 1.0f : 0.0f;
        std::memcpy(push.cluster_params, frame.cluster_params, sizeof(push.cluster_params));
        push.screen_size[0] = frame.screen_size[0];
        push.screen_size[1] = frame.screen_size[1];

        TextureView cascade_view = frame.csm_active && cascade_atlas_handle != kInvalidResource
                                       ? ctx.graph->image(cascade_atlas_handle).view
                                       : frame.local_shadow_atlas;  // any depth view; gated off
        ctx.cmd->BindPipeline(scatter_pipeline_);
        ctx.cmd->BindTransient(
            0,
            {Bind::Storage(0, scatter_[slot]),
             InGeneral(Bind::Combined(1, scatter_[slot ^ 1].view, sampler_)),
             Bind::StorageBuffer(2, frame.lights, 0, frame.lights.size),
             Bind::StorageBuffer(3, frame.cluster_counts, 0, frame.cluster_counts.size),
             Bind::StorageBuffer(4, frame.cluster_indices, 0, frame.cluster_indices.size),
             Bind::StorageBuffer(5, frame.local_shadow_faces, 0, frame.local_shadow_faces.size),
             Bind::Combined(6, frame.local_shadow_atlas, frame.comparison_sampler),
             Bind::Uniform(7, frame.cascade_buffer ? frame.cascade_buffer : dummy_uniform_, 0,
                           frame.cascade_buffer ? frame.cascade_size : 512),
             Bind::Combined(8, cascade_view, frame.comparison_sampler)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch((kSizeX + 3) / 4, (kSizeY + 3) / 4, (kSizeZ + 3) / 4);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
      });

  graph.AddPass(
      "froxel_integrate", [](RenderGraph::PassBuilder&) {},
      [this, slot](PassContext& ctx) {
        IntegratePush push{kNear, kFar, kSizeZ, 0.0f};
        ctx.cmd->BindPipeline(integrate_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, integrated_),
                                   Bind::Storage(1, scatter_[slot])});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch((kSizeX + 7) / 8, (kSizeY + 7) / 8, 1);
        // The integrated volume feeds the compute apply AND fragment-stage
        // samplers (translucents, particles, precipitation), so the write must
        // be visible to both read scopes.
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });

  graph.AddPass(
      "froxel_apply",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(lit, ResourceUsage::kStorageWrite);
        b.Read(depth_export, ResourceUsage::kSampledCompute);
      },
      [this, lit, depth_export, extent](PassContext& ctx) {
        ApplyPush push{kNear, kFar, {extent.width, extent.height}};
        ctx.cmd->BindPipeline(apply_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(lit)),
                InGeneral(Bind::Combined(1, integrated_.view, sampler_)),
                Bind::Sampled(2, ctx.graph->image(depth_export))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
}

}  // namespace rx::render
