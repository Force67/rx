#include "rxe/render/gi/restir_di.h"

#include "foundation/logging/log.h"
#include "shaders/restir_di_spatial_cs_hlsl.h"
#include "shaders/restir_di_temporal_cs_hlsl.h"

namespace rx::render {
namespace {

struct TemporalPush {
  Mat4 inv_view_proj;
  u32 size[2];
  u32 frame_index;
  u32 light_count;
  u32 candidates;
  f32 m_max;
  f32 reset;
  f32 pad0;
};

struct SpatialPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  u32 size[2];
  u32 frame_index;
  u32 light_count;
  u32 sample_count;
  f32 radius;
  f32 m_max;
  f32 pad0;
};

}  // namespace

bool RestirDi::Initialize(gpu::Device& device) {
  temporal_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_restir_di_temporal_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kSampledImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kSampledImage},
                          {4, gpu::BindingType::kSampledImage},
                          {5, gpu::BindingType::kSampledImage},
                          {6, gpu::BindingType::kSampledImage},
                          {7, gpu::BindingType::kStorageBuffer}}}},
      .push_constant_size = gpu::PushSize<TemporalPush>(),
      .debug_name = "restir_di_temporal",
  });
  spatial_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_restir_di_spatial_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kSampledImage},
                          {4, gpu::BindingType::kSampledImage},
                          {5, gpu::BindingType::kStorageBuffer},
                          {6, gpu::BindingType::kAccelStruct},
                          {7, gpu::BindingType::kStorageImage},
                          {8, gpu::BindingType::kStorageImage},
                          {9, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<SpatialPush>(),
      .debug_name = "restir_di_spatial",
  });
  if (!temporal_pipeline_ || !spatial_pipeline_) {
    RX_ERROR("restir di pipeline creation failed");
    Destroy(device);
    return false;
  }
  return true;
}

void RestirDi::Destroy(gpu::Device& device) {
  for (gpu::PipelineHandle* p : {&temporal_pipeline_, &spatial_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (gpu::GpuImage& image : reservoir_) {
    if (image) device.DestroyImage(image);
    image = {};
  }
  if (prev_depth_) device.DestroyImage(prev_depth_);
  prev_depth_ = {};
  if (prev_normal_) device.DestroyImage(prev_normal_);
  prev_normal_ = {};
}

bool RestirDi::Resize(gpu::Device& device, gpu::Extent2D extent) {
  if (!temporal_pipeline_ || !spatial_pipeline_) return false;
  if (available() && extent.width == extent_.width && extent.height == extent_.height) {
    return true;
  }
  if (available()) device.WaitIdle();
  for (gpu::GpuImage& image : reservoir_) {
    if (image) device.DestroyImage(image);
  }
  if (prev_depth_) device.DestroyImage(prev_depth_);
  if (prev_normal_) device.DestroyImage(prev_normal_);
  extent_ = extent;
  if (extent.width == 0 || extent.height == 0) return false;

  const gpu::TextureUsageFlags usage = gpu::kTextureUsageStorage | gpu::kTextureUsageSampled;
  reservoir_[0] = device.CreateImage2D(gpu::Format::kRGBA32Float, extent, usage);
  reservoir_[1] = device.CreateImage2D(gpu::Format::kRGBA32Float, extent, usage);
  prev_depth_ = device.CreateImage2D(gpu::Format::kR32Float, extent, usage);
  prev_normal_ = device.CreateImage2D(gpu::Format::kRGBA16Float, extent, usage);
  if (!reservoir_[0] || !reservoir_[1] || !prev_depth_ || !prev_normal_) {
    for (gpu::GpuImage& image : reservoir_) device.DestroyImage(image);
    device.DestroyImage(prev_depth_);
    device.DestroyImage(prev_normal_);
    RX_WARN("restir di history allocation failed");
    return false;
  }
  device.ImmediateSubmit([this](gpu::CommandList& cmd) {
    gpu::TextureBarrier to_general[4] = {
        gpu::Transition(reservoir_[0], gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(reservoir_[1], gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(prev_depth_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(prev_normal_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  reset_ = true;
  return true;
}

RestirDi::Outputs RestirDi::AddToGraph(RenderGraph& graph, ResourceHandle depth_export,
                                       ResourceHandle normals, ResourceHandle motion,
                                       RayTracingContext& raytracing, gpu::Extent2D extent,
                                       const Frame& frame) {
  Outputs out;
  if (!available() || !frame.lights || frame.light_count == 0) return out;

  out.diffuse = graph.CreateTexture({.name = "restir_di_diffuse",
                                     .format = gpu::Format::kRGBA16Float,
                                     .width = extent.width,
                                     .height = extent.height});
  out.spec = graph.CreateTexture({.name = "restir_di_spec",
                                  .format = gpu::Format::kRGBA16Float,
                                  .width = extent.width,
                                  .height = extent.height});

  const bool reset = reset_ || frame.frame_index != previous_frame_ + 1u;
  reset_ = false;
  previous_frame_ = frame.frame_index;

  graph.AddPass(
      "restir_di_temporal",
      [&](RenderGraph::PassBuilder& b) {
        b.Read(depth_export, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
        b.Read(motion, ResourceUsage::kSampledCompute);
      },
      [this, depth_export, normals, motion, extent, frame, reset](PassContext& ctx) {
        TemporalPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.frame_index = frame.frame_index;
        push.light_count = frame.light_count;
        push.candidates = 8;
        push.m_max = 20.0f;
        push.reset = reset ? 1.0f : 0.0f;

        ctx.cmd->BindPipeline(temporal_pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Storage(0, reservoir_[0]),
                gpu::Bind::Sampled(1, ctx.graph->image(depth_export)),
                gpu::Bind::Sampled(2, ctx.graph->image(normals)),
                gpu::Bind::Sampled(3, ctx.graph->image(motion)),
                gpu::InGeneral(gpu::Bind::Sampled(4, prev_depth_)),
                gpu::InGeneral(gpu::Bind::Sampled(5, prev_normal_)),
                gpu::InGeneral(gpu::Bind::Sampled(6, reservoir_[1])),
                gpu::Bind::StorageBuffer(7, frame.lights, 0, frame.lights.size)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);
      });

  graph.AddPass(
      "restir_di_spatial",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out.diffuse, ResourceUsage::kStorageWrite);
        b.Write(out.spec, ResourceUsage::kStorageWrite);
        b.Read(depth_export, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
      },
      [this, out, depth_export, normals, &raytracing, extent, frame](PassContext& ctx) {
        SpatialPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = 1.0f;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.frame_index = frame.frame_index;
        push.light_count = frame.light_count;
        push.sample_count = 4;
        push.radius = 16.0f;
        push.m_max = 60.0f;

        ctx.cmd->BindPipeline(spatial_pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Storage(0, ctx.graph->image(out.diffuse)),
                gpu::Bind::Storage(1, ctx.graph->image(out.spec)),
                gpu::InGeneral(gpu::Bind::Sampled(2, reservoir_[0])),
                gpu::Bind::Sampled(3, ctx.graph->image(depth_export)),
                gpu::Bind::Sampled(4, ctx.graph->image(normals)),
                gpu::Bind::StorageBuffer(5, frame.lights, 0, frame.lights.size),
                gpu::Bind::Accel(6, raytracing.tlas(frame.tlas_slot)),
                gpu::Bind::Storage(7, reservoir_[1]),
                gpu::Bind::Storage(8, prev_depth_),
                gpu::Bind::Storage(9, prev_normal_)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);
      });

  return out;
}

}  // namespace rx::render
