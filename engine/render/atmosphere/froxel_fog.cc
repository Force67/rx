#include "render/atmosphere/froxel_fog.h"

#include <cstring>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "shaders/froxel_apply_cs_hlsl.h"
#include "shaders/froxel_integrate_cs_hlsl.h"
#include "shaders/froxel_scatter_cs_hlsl.h"
#include "shaders/froxel_scatter_rt_cs_hlsl.h"

namespace rx::render {
namespace {

// The two matrices on their own are the entire 128 bytes vulkan guarantees for
// a push block, so they ride in a per-frame uniform buffer and the push keeps
// the scalars.
struct ScatterCamera {
  Mat4 inv_view_proj;  // unjittered
  Mat4 prev_view_proj;
};
struct ScatterPush {
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

bool FroxelFog::Initialize(Device& device, bool ray_query) {
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
                          {8, BindingType::kCombinedTextureSampler},
                          {10, BindingType::kUniformBuffer}}}},
      .push_constant_size = PushSize<ScatterPush>(),
      .debug_name = "froxel_scatter",
  });
  if (ray_query) {
    // Same set plus the TLAS at 9. A failure here is nonfatal: the pass falls
    // back to the cascade pipeline, which is what non-rt devices run anyway.
    scatter_pipeline_rt_ = device.CreateComputePipeline({
        .shader = RX_SHADER(k_froxel_scatter_rt_cs_hlsl),
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kCombinedTextureSampler},
                            {2, BindingType::kStorageBuffer},
                            {3, BindingType::kStorageBuffer},
                            {4, BindingType::kStorageBuffer},
                            {5, BindingType::kStorageBuffer},
                            {6, BindingType::kCombinedTextureSampler},
                            {7, BindingType::kUniformBuffer},
                            {8, BindingType::kCombinedTextureSampler},
                            {9, BindingType::kAccelStruct},
                            {10, BindingType::kUniformBuffer}}}},
        .push_constant_size = PushSize<ScatterPush>(),
        .debug_name = "froxel_scatter_rt",
    });
    if (!scatter_pipeline_rt_)
      RX_WARN("froxel scatter rt variant unavailable; fog sun stays cascade-shadowed");
  }
  integrate_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_froxel_integrate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage}}}},
      .push_constant_size = PushSize<IntegratePush>(),
      .debug_name = "froxel_integrate",
  });
  apply_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_froxel_apply_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kSampledImage}}}},
      .push_constant_size = PushSize<ApplyPush>(),
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

  // One per in-flight frame: the pass rewrites it while the previous frame may
  // still be reading its own copy.
  for (GpuBuffer& camera : camera_) {
    camera = device.CreateBuffer(sizeof(ScatterCamera), kBufferUsageUniform, true);
    if (!camera.mapped) return false;
  }

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
  for (PipelineHandle* p : {&scatter_pipeline_, &scatter_pipeline_rt_, &integrate_pipeline_,
                            &apply_pipeline_}) {
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
  for (GpuBuffer& camera : camera_) {
    if (camera) device.DestroyBuffer(camera);
    camera = {};
  }
}

void FroxelFog::AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle depth_export,
                           ResourceHandle cascade_atlas_handle, RayTracingContext* raytracing,
                           u32 tlas_slot, Extent2D extent, const Frame& frame) {
  const u32 slot = frame.frame_index % 2;
  const bool rt = frame.ray_query_sun && static_cast<bool>(scatter_pipeline_rt_) && raytracing &&
                  raytracing->TlasValid(tlas_slot);

  graph.AddPass(
      "froxel_scatter",
      [&](RenderGraph::PassBuilder& b) {
        if (cascade_atlas_handle != kInvalidResource)
          b.Read(cascade_atlas_handle, ResourceUsage::kSampledCompute);
      },
      [this, slot, cascade_atlas_handle, raytracing, tlas_slot, rt, frame](PassContext& ctx) {
        const ScatterCamera camera{frame.inv_view_proj, frame.prev_view_proj};
        std::memcpy(camera_[slot].mapped, &camera, sizeof(camera));

        ScatterPush push{};
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
        // 0 none / 1 cascade / 2 ray query, matching the shader's constants.
        push.volume_params[3] = rt ? 2.0f : (frame.csm_active ? 1.0f : 0.0f);
        std::memcpy(push.cluster_params, frame.cluster_params, sizeof(push.cluster_params));
        push.screen_size[0] = frame.screen_size[0];
        push.screen_size[1] = frame.screen_size[1];
        push.screen_size[3] = frame.start_distance;

        TextureView cascade_view = frame.csm_active && cascade_atlas_handle != kInvalidResource
                                       ? ctx.graph->image(cascade_atlas_handle).view
                                       : frame.local_shadow_atlas;  // any depth view; gated off
        ctx.cmd->BindPipeline(rt ? scatter_pipeline_rt_ : scatter_pipeline_);
        base::Vector<BindingItem> items = {
            Bind::Storage(0, scatter_[slot]),
            InGeneral(Bind::Combined(1, scatter_[slot ^ 1].view, sampler_)),
            Bind::StorageBuffer(2, frame.lights, 0, frame.lights.size),
            Bind::StorageBuffer(3, frame.cluster_counts, 0, frame.cluster_counts.size),
            Bind::StorageBuffer(4, frame.cluster_indices, 0, frame.cluster_indices.size),
            Bind::StorageBuffer(5, frame.local_shadow_faces, 0, frame.local_shadow_faces.size),
            Bind::Combined(6, frame.local_shadow_atlas, frame.comparison_sampler),
            Bind::Uniform(7, frame.cascade_buffer ? frame.cascade_buffer : dummy_uniform_, 0,
                          frame.cascade_buffer ? frame.cascade_size : 512),
            Bind::Combined(8, cascade_view, frame.comparison_sampler)};
        if (rt) items.push_back(Bind::Accel(9, raytracing->tlas(tlas_slot)));
        items.push_back(Bind::Uniform(10, camera_[slot], 0, sizeof(ScatterCamera)));
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
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
