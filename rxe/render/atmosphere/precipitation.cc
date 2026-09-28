#include "rxe/render/atmosphere/precipitation.h"

#include "foundation/logging/log.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/precipitation_cs_hlsl.h"

namespace rx::render {
namespace {

struct PrecipPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];  // xyz eye, w time
  f32 params[4];      // intensity, snow, unused, unused
  u32 size[2];
  u32 pad[2];
};

}  // namespace

bool Precipitation::Initialize(gpu::Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_precipitation_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kSampledImage}}}},
      .push_constant_size = gpu::PushSize<PrecipPush>(),
      .debug_name = "precipitation",
  });
  if (!pipeline_) {
    RX_ERROR("precipitation pipeline creation failed");
    return false;
  }
  return true;
}

void Precipitation::Destroy(gpu::Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle Precipitation::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                         gpu::Extent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "precipitation",
                                            .format = gpu::Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "precipitation",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, out, extent, frame](PassContext& ctx) {
        PrecipPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = frame.time;
        push.params[0] = frame.intensity;
        push.params[1] = frame.snow ? 1.0f : 0.0f;
        push.size[0] = extent.width;
        push.size[1] = extent.height;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, ctx.graph->image(out)),
                                   gpu::Bind::Sampled(1, ctx.graph->image(color))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rx::render
