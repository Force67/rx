#include "render/atmosphere/surface_weather.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/surface_weather_cs_hlsl.h"

namespace rx::render {
namespace {

struct SurfacePush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];  // xyz eye
  f32 params[4];      // wetness, snow cover, time, live rain
  f32 occl[4];        // sky-occlusion: center xz, 1/half extent, top_y
  f32 occl2[4];       // x y-range (<= 0 disables), yzw unused
  u32 size[2];
  u32 pad[2];
};

}  // namespace

bool SurfaceWeather::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_surface_weather_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kCombinedTextureSampler},
                          {5, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(SurfacePush),
      .debug_name = "surface_weather",
  });
  if (!pipeline_) {
    RX_ERROR("surface weather pipeline creation failed");
    return false;
  }
  return true;
}

void SurfaceWeather::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

ResourceHandle SurfaceWeather::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                          ResourceHandle normals, ResourceHandle depth,
                                          TextureView sky_view, SamplerHandle sky_sampler,
                                          Extent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "surface_weather",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "surface_weather",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, normals, depth, out, sky_view, sky_sampler, extent, frame](PassContext& ctx) {
        SurfacePush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.params[0] = frame.wetness;
        push.params[1] = frame.snow_cover;
        push.params[2] = frame.time;
        push.params[3] = frame.rain;
        std::memcpy(push.occl, frame.occl, sizeof(push.occl));
        push.occl2[0] = frame.occlusion ? frame.occl_range : 0.0f;
        push.size[0] = extent.width;
        push.size[1] = extent.height;

        // The occlusion slot must always be bound; without a live map the
        // shader is told to ignore it (occl2.x <= 0) but a valid 2D view is
        // still required, so the scene depth stands in.
        BindingItem occl = frame.occlusion
                               ? Bind::Combined(5, frame.occlusion, frame.occlusion_sampler)
                               : Bind::Combined(5, ctx.graph->image(depth).view, sky_sampler);
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(out)),
                                   Bind::Sampled(1, ctx.graph->image(color)),
                                   Bind::Sampled(2, ctx.graph->image(normals)),
                                   Bind::Sampled(3, ctx.graph->image(depth)),
                                   Bind::Combined(4, sky_view, sky_sampler), occl});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rx::render
