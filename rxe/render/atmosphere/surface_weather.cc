#include "rxe/render/atmosphere/surface_weather.h"

#include <string.h>

#include "base/memory/mem_ops.h"
#include "foundation/logging/log.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/surface_weather_cs_hlsl.h"

namespace rx::render {
namespace {

// Half of the 128 bytes vulkan guarantees for a push block would go to this one
// matrix, so it rides in a per-frame uniform buffer and the push keeps the
// scalars.
struct SurfaceCamera {
  Mat4 inv_view_proj;
};
struct SurfacePush {
  f32 camera_pos[4];  // xyz eye
  f32 params[4];      // wetness, snow cover, time, live rain
  f32 occl[4];        // sky-occlusion: center xz, 1/half extent, top_y
  f32 occl2[4];       // x y-range (<= 0 disables), yzw unused
  u32 size[2];
  u32 pad[2];
};

}  // namespace

bool SurfaceWeather::Initialize(gpu::Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_surface_weather_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kSampledImage},
                          {2, gpu::BindingType::kSampledImage},
                          {3, gpu::BindingType::kSampledImage},
                          {4, gpu::BindingType::kCombinedTextureSampler},
                          {5, gpu::BindingType::kCombinedTextureSampler},
                          {6, gpu::BindingType::kUniformBuffer}}}},
      .push_constant_size = gpu::PushSize<SurfacePush>(),
      .debug_name = "surface_weather",
  });
  if (!pipeline_) {
    RX_ERROR("surface weather pipeline creation failed");
    return false;
  }
  // One per in-flight frame: the pass rewrites it while the previous frame may
  // still be reading its own copy.
  for (gpu::GpuBuffer& camera : camera_) {
    camera = device.CreateBuffer(sizeof(SurfaceCamera), gpu::kBufferUsageUniform, true);
    if (!camera.mapped) return false;
  }
  return true;
}

void SurfaceWeather::Destroy(gpu::Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (gpu::GpuBuffer& camera : camera_) {
    if (camera) device.DestroyBuffer(camera);
    camera = {};
  }
}

ResourceHandle SurfaceWeather::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                          ResourceHandle normals, ResourceHandle depth,
                                          gpu::TextureView sky_view, gpu::SamplerHandle sky_sampler,
                                          gpu::Extent2D extent, const Frame& frame) {
  ResourceHandle out = graph.CreateTexture({.name = "surface_weather",
                                            .format = gpu::Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  uniform_slot_ ^= 1;
  const u32 slot = uniform_slot_;
  graph.AddPass(
      "surface_weather",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, color, normals, depth, out, sky_view, sky_sampler, extent, frame,
       slot](PassContext& ctx) {
        const SurfaceCamera camera{frame.inv_view_proj};
        base::MemCopy(camera_[slot].mapped, &camera, sizeof(camera));

        SurfacePush push{};
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.params[0] = frame.wetness;
        push.params[1] = frame.snow_cover;
        push.params[2] = frame.time;
        push.params[3] = frame.rain;
        base::MemCopy(push.occl, frame.occl, sizeof(push.occl));
        push.occl2[0] = frame.occlusion ? frame.occl_range : 0.0f;
        push.size[0] = extent.width;
        push.size[1] = extent.height;

        // The occlusion slot must always be bound; without a live map the
        // shader is told to ignore it (occl2.x <= 0) but a valid 2D view is
        // still required, so the scene depth stands in.
        gpu::BindingItem occl = frame.occlusion
                               ? gpu::Bind::Combined(5, frame.occlusion, frame.occlusion_sampler)
                               : gpu::Bind::Combined(5, ctx.graph->image(depth).view, sky_sampler);
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, ctx.graph->image(out)),
                                   gpu::Bind::Sampled(1, ctx.graph->image(color)),
                                   gpu::Bind::Sampled(2, ctx.graph->image(normals)),
                                   gpu::Bind::Sampled(3, ctx.graph->image(depth)),
                                   gpu::Bind::Combined(4, sky_view, sky_sampler), occl,
                                   gpu::Bind::Uniform(6, camera_[slot], 0, sizeof(SurfaceCamera))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rx::render
