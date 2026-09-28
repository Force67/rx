#include "rxe/render/geometry/wboit.h"

#include <string.h>

#include "base/memory/mem_ops.h"
#include "foundation/logging/log.h"
#include "rxe/asset/primitives.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/fullscreen_vs_slang.h"
#include "shaders/wboit_ps_hlsl.h"
#include "shaders/wboit_resolve_ps_hlsl.h"
#include "shaders/wboit_vs_hlsl.h"

namespace rx::render {
namespace {

constexpr gpu::Format kAccumFormat = gpu::Format::kRGBA16Float;
constexpr gpu::Format kRevealFormat = gpu::Format::kR16Float;

// Only model and color differ between the instance draws. The view projection
// and the two lighting parameter blocks are the same for the whole pass, and
// keeping them in the push would exceed the 128 bytes vulkan guarantees, so
// they ride in a per-frame uniform buffer.
struct WboitFrame {
  Mat4 view_proj;
  f32 cluster_params[4];
  f32 froxel_params[4];
};

struct WboitPush {
  Mat4 model;
  f32 color[4];
  f32 sun_dir[3];
  f32 pad0;
  f32 sun_color[3];
  f32 ambient;
};

}  // namespace

bool WboitPass::Initialize(gpu::Device& device, gpu::Format color_format, gpu::Format depth_format) {
  color_format_ = color_format;
  asset::Mesh sphere = asset::MakeSphere(1.0f, 40, 60, asset::MakeAssetId("builtin/oit/sphere"));
  const asset::MeshLod& lod = sphere.lods[0];
  index_count_ = static_cast<u32>(lod.indices.size());
  vertices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      gpu::kBufferUsageVertex);
  indices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      gpu::kBufferUsageIndex);

  // Geometry pipeline: accumulate into the two oit targets.
  // accum: additive. revealage: dst *= (1 - src.r).
  geom_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_wboit_vs_hlsl),
      .fragment = RX_SHADER(k_wboit_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{0, gpu::Format::kRGB32Float,
                                          offsetof(asset::Vertex, position)},
                                         {1, gpu::Format::kRGB32Float,
                                          offsetof(asset::Vertex, normal)}}}},
      .raster = {.cull = gpu::CullMode::kNone,  // see through both faces
                 .front = gpu::FrontFace::kCounterClockwise},
      .depth = {.test = true,
                .write = false,
                .compare = gpu::CompareOp::kGreaterEqual,  // reversed z, occluded by opaque
                .format = depth_format},
      .color_formats = {kAccumFormat, kRevealFormat},
      .blend = {gpu::BlendMode::kWboitAccum, gpu::BlendMode::kWboitReveal},
      .sets = {{.slots = {{0, gpu::BindingType::kStorageBuffer},
                          {1, gpu::BindingType::kStorageBuffer},
                          {2, gpu::BindingType::kStorageBuffer},
                          {3, gpu::BindingType::kCombinedTextureSampler},
                          {4, gpu::BindingType::kUniformBuffer}}}},
      .push_constant_size = gpu::PushSize<WboitPush>(),
      .debug_name = "wboit_geom",
  });
  if (!geom_pipeline_) {
    RX_ERROR("wboit geometry pipeline creation failed");
    return false;
  }

  // One per in-flight frame: the pass rewrites it while the previous frame may
  // still be reading its own copy.
  for (gpu::GpuBuffer& frame : frames_) {
    frame = device.CreateBuffer(sizeof(WboitFrame), gpu::kBufferUsageUniform, true);
    if (!frame.mapped) {
      RX_ERROR("wboit frame uniform allocation failed");
      return false;
    }
  }

  // Resolve pipeline: composite the oit targets over the scene.
  sampler_ = device.GetSampler({.min_filter = gpu::Filter::kNearest,
                                .mag_filter = gpu::Filter::kNearest,
                                .mip_filter = gpu::Filter::kNearest,
                                .address_u = gpu::AddressMode::kClampToEdge,
                                .address_v = gpu::AddressMode::kClampToEdge,
                                .address_w = gpu::AddressMode::kClampToEdge});

  resolve_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fullscreen_vs_slang),
      .fragment = RX_SHADER(k_wboit_resolve_ps_hlsl),
      .raster = {.cull = gpu::CullMode::kNone, .front = gpu::FrontFace::kCounterClockwise},
      .color_formats = {color_format_},
      .blend = {gpu::BlendMode::kOpaque},
      .sets = {{.slots = {{0, gpu::BindingType::kCombinedTextureSampler},
                          {1, gpu::BindingType::kCombinedTextureSampler},
                          {2, gpu::BindingType::kCombinedTextureSampler}},
                .stages = gpu::kShaderStageFragment}},
      .debug_name = "wboit_resolve",
  });
  if (!resolve_pipeline_) {
    RX_ERROR("wboit resolve pipeline creation failed");
    return false;
  }
  return true;
}

ResourceHandle WboitPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                     const base::Vector<WboitInstance>& instances,
                                     const Mat4& view_proj, const Vec3& sun_dir,
                                     const Vec3& sun_color, f32 ambient, u32 width, u32 height, const LightingContext& lighting) {
  ResourceHandle accum =
      graph.CreateTexture({.name = "oit_accum", .format = kAccumFormat, .width = width, .height = height});
  ResourceHandle reveal =
      graph.CreateTexture({.name = "oit_reveal", .format = kRevealFormat, .width = width, .height = height});
  ResourceHandle composite = graph.CreateTexture(
      {.name = "oit_composite", .format = color_format_, .width = width, .height = height});

  WboitPush base{};
  base.sun_dir[0] = sun_dir.x;
  base.sun_dir[1] = sun_dir.y;
  base.sun_dir[2] = sun_dir.z;
  base.sun_color[0] = sun_color.x;
  base.sun_color[1] = sun_color.y;
  base.sun_color[2] = sun_color.z;
  base.ambient = ambient;

  WboitFrame constants{};
  constants.view_proj = view_proj;
  base::MemCopy(constants.cluster_params, lighting.cluster_params,
              sizeof(constants.cluster_params));
  constants.froxel_params[0] = lighting.froxel_near;
  constants.froxel_params[1] = lighting.froxel_far;
  constants.froxel_params[2] = lighting.froxel_enabled ? 1.0f : 0.0f;
  constants.froxel_params[3] = 0.0f;
  frame_slot_ ^= 1u;
  const u32 slot = frame_slot_;

  graph.AddPass(
      "oit_accumulate",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(accum, ResourceUsage::kColorAttachment);
        builder.Write(reveal, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, accum, reveal, depth, instances, base, constants, slot, width, height,
       lighting](PassContext& ctx) {
        gpu::ColorAttachment colors[2];
        colors[0] = {.view = ctx.graph->image(accum).view,
                     .load = gpu::LoadOp::kClear,
                     .clear = {0.0f, 0.0f, 0.0f, 0.0f}};
        colors[1] = {.view = ctx.graph->image(reveal).view,
                     .load = gpu::LoadOp::kClear,
                     .clear = {1.0f, 0.0f, 0.0f, 0.0f}};  // full transmittance
        gpu::DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = gpu::LoadOp::kLoad};

        ctx.cmd->BeginRendering(
            {.extent = {width, height}, .colors = colors, .depth = &depth_att});
        ctx.cmd->BindPipeline(geom_pipeline_);
        base::MemCopy(frames_[slot].mapped, &constants, sizeof(constants));
        ctx.cmd->BindTransient(
            0, {gpu::Bind::StorageBuffer(0, lighting.lights, 0, lighting.lights.size),
                gpu::Bind::StorageBuffer(1, lighting.cluster_counts, 0, lighting.cluster_counts.size),
                gpu::Bind::StorageBuffer(2, lighting.cluster_indices, 0,
                                    lighting.cluster_indices.size),
                gpu::InGeneral(gpu::Bind::Combined(3, lighting.froxel_volume, lighting.froxel_sampler)),
                gpu::Bind::Uniform(4, frames_[slot], 0, sizeof(WboitFrame))});
        ctx.cmd->BindVertexBuffer(0, vertices_, 0);
        ctx.cmd->BindIndexBuffer(indices_, 0, gpu::IndexType::kUint32);
        for (const WboitInstance& inst : instances) {
          WboitPush push = base;
          push.model = inst.model;
          push.color[0] = inst.color[0];
          push.color[1] = inst.color[1];
          push.color[2] = inst.color[2];
          push.color[3] = inst.color[3];
          ctx.cmd->Push(push);
          ctx.cmd->DrawIndexed(index_count_, 1, 0, 0, 0);
        }
        ctx.cmd->EndRendering();
      });

  graph.AddPass(
      "oit_resolve",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(accum, ResourceUsage::kSampledFragment);
        builder.Read(reveal, ResourceUsage::kSampledFragment);
        builder.Read(color, ResourceUsage::kSampledFragment);
        builder.Write(composite, ResourceUsage::kColorAttachment);
      },
      [this, accum, reveal, color, composite, width, height](PassContext& ctx) {
        gpu::ColorAttachment att[] = {{.view = ctx.graph->image(composite).view,
                                  .load = gpu::LoadOp::kDontCare}};
        ctx.cmd->BeginRendering({.extent = {width, height}, .colors = att});
        ctx.cmd->BindPipeline(resolve_pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Combined(0, ctx.graph->image(accum).view, sampler_),
                gpu::Bind::Combined(1, ctx.graph->image(reveal).view, sampler_),
                gpu::Bind::Combined(2, ctx.graph->image(color).view, sampler_)});
        ctx.cmd->Draw(3, 1, 0, 0);
        ctx.cmd->EndRendering();
      });
  return composite;
}

void WboitPass::Destroy(gpu::Device& device) {
  device.DestroyPipeline(geom_pipeline_);
  geom_pipeline_ = {};
  device.DestroyPipeline(resolve_pipeline_);
  resolve_pipeline_ = {};
  device.DestroyBuffer(vertices_);
  device.DestroyBuffer(indices_);
  for (gpu::GpuBuffer& frame : frames_) {
    if (frame) device.DestroyBuffer(frame);
    frame = {};
  }
}

}  // namespace rx::render
