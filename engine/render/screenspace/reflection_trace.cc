#include "render/screenspace/reflection_trace.h"

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "render/rhi/device.h"
#include "shaders/reflection_trace_cs_hlsl.h"
#include "shaders/reflection_upscale_cs_hlsl.h"

namespace rx::render {
namespace {

struct ReflectionPush {
  Mat4 inv_view_proj;
  f32 camera_pos[4];
  f32 sun_direction[4];
  f32 sun_color[4];
  f32 fog[4];  // density, height falloff, base height, unused
  f32 inv_size[2];
  f32 near_plane;
  f32 roughness_cutoff;
  f32 hit_a;
  f32 hit_b;
  f32 hit_c;
  f32 frame_index;
  f32 max_ray_dist;
  f32 sh_skip_rough;
  f32 sh_dir_thresh;
  f32 fog_mip;
  u32 dims[4];  // out_w, out_h, sh_gather_w, sh_gather_h
  u32 misc[4];  // guide sample step, flags, pad, pad
};

struct UpscalePush {
  u32 dims[4];    // full_w, full_h, half_w, half_h
  f32 params[4];  // near_plane, ...
};

constexpr u32 kFlagDdgi = 1u;
constexpr u32 kFlagVegAnyHit = 2u;
constexpr u32 kFlagFog = 4u;
constexpr u32 kFlagShSkip = 8u;

// The push blocks must fit the guaranteed 128-byte range on every backend plus
// the engine's 256-byte cap, and stay 16-byte aligned so the HLSL cbuffer layout
// matches the C++ layout field-for-field.
static_assert(sizeof(ReflectionPush) == 208, "ReflectionPush must match the shader layout");
static_assert(sizeof(ReflectionPush) <= 256, "ReflectionPush exceeds the push-constant cap");
static_assert(sizeof(UpscalePush) == 32, "UpscalePush must match the shader layout");

}  // namespace

bool ReflectionTrace::Initialize(Device& device, BindingLayoutHandle bindless_layout) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_reflection_trace_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kAccelStruct},
                          {4, BindingType::kCombinedTextureSampler},
                          {5, BindingType::kCombinedTextureSampler},
                          {6, BindingType::kUniformBuffer},
                          {7, BindingType::kSampledImage},
                          {8, BindingType::kSampledImage},
                          {9, BindingType::kSampledImage}}},
               {.shared = bindless_layout}},
      .push_constant_size = sizeof(ReflectionPush),
      .debug_name = "reflection_trace",
  });
  if (!pipeline_) {
    RX_ERROR("reflection trace pipeline creation failed");
    return false;
  }
  upscale_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_reflection_upscale_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(UpscalePush),
      .debug_name = "reflection_upscale",
  });
  if (!upscale_pipeline_) {
    RX_ERROR("reflection upscale pipeline creation failed");
    return false;
  }
  return true;
}

void ReflectionTrace::Destroy(Device& device) {
  if (pipeline_) {
    device.DestroyPipeline(pipeline_);
    pipeline_ = {};
  }
  if (upscale_pipeline_) {
    device.DestroyPipeline(upscale_pipeline_);
    upscale_pipeline_ = {};
  }
}

ResourceHandle ReflectionTrace::AddToGraph(
    RenderGraph& graph, RayTracingContext& raytracing, u32 tlas_slot, BindingSetHandle bindless_set,
    ResourceHandle depth, ResourceHandle normals, TextureView prefiltered,
    TextureView ddgi_irradiance, bool ddgi_in_general, const GpuBuffer& ddgi_volume,
    u64 ddgi_volume_size, SamplerHandle sampler, Extent2D extent, ResourceHandle sh_r,
    ResourceHandle sh_g, ResourceHandle sh_b, Extent2D sh_extent, const Frame& frame) {
  // Trace at half resolution when requested (quarters the ray count). Guides
  // stay full-res; the shader maps each half-res pixel to a full-res texel.
  const u32 step = frame.half_res ? 2u : 1u;
  Extent2D trace{(extent.width + step - 1) / step, (extent.height + step - 1) / step};
  const bool sh_valid = sh_r != kInvalidResource && sh_g != kInvalidResource &&
                        sh_b != kInvalidResource && frame.sh_skip;

  ResourceHandle raw = graph.CreateTexture({.name = "spec_reflection_raw",
                                            .format = Format::kRGBA16Float,
                                            .width = trace.width,
                                            .height = trace.height});
  // Placeholder for the SH slots when RCGI is off (never read: flag stays clear).
  ResourceHandle sh0 = sh_valid ? sh_r : normals;
  ResourceHandle sh1 = sh_valid ? sh_g : normals;
  ResourceHandle sh2 = sh_valid ? sh_b : normals;
  graph.AddPass(
      "reflection_trace",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(raw, ResourceUsage::kStorageWrite);
        b.Read(depth, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
        if (sh_valid) {
          b.Read(sh_r, ResourceUsage::kSampledCompute);
          b.Read(sh_g, ResourceUsage::kSampledCompute);
          b.Read(sh_b, ResourceUsage::kSampledCompute);
        }
      },
      [this, &raytracing, tlas_slot, bindless_set, raw, depth, normals, prefiltered, ddgi_irradiance,
       ddgi_in_general, ddgi_volume, ddgi_volume_size, sampler, extent, trace, step, sh_valid, sh0,
       sh1, sh2, sh_extent, frame](PassContext& ctx) {
        BindingItem ddgi_item = Bind::Combined(5, ddgi_irradiance, sampler);
        if (ddgi_in_general) ddgi_item = InGeneral(ddgi_item);
        base::Vector<BindingItem> items;
        items.push_back(Bind::Storage(0, ctx.graph->image(raw)));
        items.push_back(Bind::Sampled(1, ctx.graph->image(depth)));
        items.push_back(Bind::Sampled(2, ctx.graph->image(normals)));
        items.push_back(Bind::Accel(3, raytracing.tlas(tlas_slot)));
        items.push_back(Bind::Combined(4, prefiltered, sampler));
        items.push_back(ddgi_item);
        items.push_back(Bind::Uniform(6, ddgi_volume, 0, ddgi_volume_size));
        items.push_back(Bind::Sampled(7, ctx.graph->image(sh0)));
        items.push_back(Bind::Sampled(8, ctx.graph->image(sh1)));
        items.push_back(Bind::Sampled(9, ctx.graph->image(sh2)));

        ReflectionPush p{};
        p.inv_view_proj = frame.inv_view_proj;
        p.camera_pos[0] = frame.camera_pos.x;
        p.camera_pos[1] = frame.camera_pos.y;
        p.camera_pos[2] = frame.camera_pos.z;
        Vec3 sun = Normalize(frame.sun_direction);
        p.sun_direction[0] = sun.x;
        p.sun_direction[1] = sun.y;
        p.sun_direction[2] = sun.z;
        p.sun_direction[3] = frame.sun_intensity;
        p.sun_color[0] = frame.sun_color.x;
        p.sun_color[1] = frame.sun_color.y;
        p.sun_color[2] = frame.sun_color.z;
        const bool fog_on = frame.fog && frame.fog_density > 0.0f;
        p.fog[0] = frame.fog_density;
        p.fog[1] = frame.fog_height_falloff;
        p.fog[2] = frame.fog_base_height;
        // Full-res inverse size: guides are always full res.
        p.inv_size[0] = 1.0f / static_cast<f32>(extent.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(extent.height);
        p.near_plane = frame.near_plane;
        p.roughness_cutoff = frame.roughness_cutoff;
        p.hit_a = frame.hit_dist_params[0];
        p.hit_b = frame.hit_dist_params[1];
        p.hit_c = frame.hit_dist_params[2];
        p.frame_index = static_cast<f32>(frame.frame_index % 64u);
        p.max_ray_dist = frame.max_ray_dist;
        p.sh_skip_rough = frame.sh_skip_roughness;
        p.sh_dir_thresh = frame.sh_dir_threshold;
        p.fog_mip = 4.0f;
        p.dims[0] = trace.width;
        p.dims[1] = trace.height;
        p.dims[2] = sh_extent.width;
        p.dims[3] = sh_extent.height;
        p.misc[0] = step;
        p.misc[1] = (frame.ddgi ? kFlagDdgi : 0u) | (frame.veg_anyhit ? kFlagVegAnyHit : 0u) |
                    (fog_on ? kFlagFog : 0u) | (sh_valid ? kFlagShSkip : 0u);
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->BindSet(1, bindless_set);
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(trace);
      });

  if (!frame.half_res) return raw;

  // Bilateral upscale half-res -> full-res before NRD consumes it.
  ResourceHandle full = graph.CreateTexture({.name = "spec_reflection_full",
                                             .format = Format::kRGBA16Float,
                                             .width = extent.width,
                                             .height = extent.height});
  graph.AddPass(
      "reflection_upscale",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(full, ResourceUsage::kStorageWrite);
        b.Read(raw, ResourceUsage::kSampledCompute);
        b.Read(depth, ResourceUsage::kSampledCompute);
        b.Read(normals, ResourceUsage::kSampledCompute);
      },
      [this, full, raw, depth, normals, extent, trace, frame](PassContext& ctx) {
        UpscalePush p{};
        p.dims[0] = extent.width;
        p.dims[1] = extent.height;
        p.dims[2] = trace.width;
        p.dims[3] = trace.height;
        p.params[0] = frame.near_plane;
        ctx.cmd->BindPipeline(upscale_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(full)),
                                   Bind::Sampled(1, ctx.graph->image(raw)),
                                   Bind::Sampled(2, ctx.graph->image(depth)),
                                   Bind::Sampled(3, ctx.graph->image(normals))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  return full;
}

}  // namespace rx::render
