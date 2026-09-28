#include "rxe/render/atmosphere/aerial_perspective.h"

#include "foundation/logging/log.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/aerial_perspective_cs_hlsl.h"

namespace rx::render {
namespace {

struct ApPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];     // xyz eye, w strength
  f32 sun_direction[4];  // xyz travel dir, w intensity
  f32 sun_color[4];      // rgb
  u32 size[2];
  u32 steps;
  u32 pad;
};

}  // namespace

bool AerialPerspective::Initialize(gpu::Device& device) {
  sampler_ = device.GetSampler({.min_filter = gpu::Filter::kLinear,
                                .mag_filter = gpu::Filter::kLinear,
                                .mip_filter = gpu::Filter::kNearest,
                                .address_u = gpu::AddressMode::kClampToEdge,
                                .address_v = gpu::AddressMode::kClampToEdge,
                                .address_w = gpu::AddressMode::kClampToEdge,
                                .max_lod = 0.0f});
  if (!sampler_) return false;

  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_aerial_perspective_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kSampledImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kCombinedTextureSampler},
                          {4, gpu::BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = gpu::PushSize<ApPush>(),
      .debug_name = "aerial_perspective",
  });
  if (!pipeline_) {
    RX_ERROR("aerial perspective pipeline creation failed");
    return false;
  }
  return true;
}

void AerialPerspective::Destroy(gpu::Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  sampler_ = {};  // cached by the device, not destroyed by callers
}

ResourceHandle AerialPerspective::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                             ResourceHandle depth, gpu::TextureView transmittance,
                                             gpu::TextureView multiscatter, gpu::Extent2D extent,
                                             const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "aerial_perspective",
                                            .format = gpu::Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "aerial_perspective",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, depth, out, transmittance, multiscatter, extent, frame](PassContext& ctx) {
        ApPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = frame.strength;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_direction[0] = sun.x;
        push.sun_direction[1] = sun.y;
        push.sun_direction[2] = sun.z;
        push.sun_direction[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.steps = frame.steps;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, ctx.graph->image(out)),
                                   gpu::Bind::Sampled(1, ctx.graph->image(color)),
                                   gpu::Bind::Sampled(2, ctx.graph->image(depth)),
                                   gpu::Bind::Combined(3, transmittance, sampler_),
                                   gpu::Bind::Combined(4, multiscatter, sampler_)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rx::render
