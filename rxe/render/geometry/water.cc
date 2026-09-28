#include "rxe/render/geometry/water.h"

#include "base/memory/unique_pointer.h"
#include "foundation/logging/log.h"
#include "rxe/asset/mesh.h"
#include "rxe/render/pipeline/mesh_pipeline.h"
#include "shaders/copy_cs_hlsl.h"
#include "shaders/mesh_vs_hlsl.h"
#include "shaders/water_ps_hlsl.h"

namespace rx::render {

base::UniquePointer<WaterPass> WaterPass::Create(gpu::Device& device, gpu::Format color_format,
                                             gpu::Format motion_format, gpu::Format depth_format,
                                             gpu::BindingLayoutHandle globals_layout,
                                             gpu::BindingLayoutHandle material_layout,
                                             gpu::BindingLayoutHandle environment_layout,
                                             gpu::BindingLayoutHandle bindless_layout) {
  auto pass = base::UniquePointer<WaterPass>(new WaterPass(device));

  pass->sampler_ = device.GetSampler({.address_u = gpu::AddressMode::kClampToEdge,
                                      .address_v = gpu::AddressMode::kClampToEdge,
                                      .address_w = gpu::AddressMode::kClampToEdge});

  // Water replaces its pixels (refraction samples the snapshot), so it
  // renders opaquely over the scene and writes depth for the post stack.
  pass->pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_mesh_vs_hlsl),
      .fragment = RX_SHADER(k_water_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{0, gpu::Format::kRGB32Float,
                                          offsetof(asset::Vertex, position)},
                                         {1, gpu::Format::kRGB32Float, offsetof(asset::Vertex, normal)},
                                         {2, gpu::Format::kRGBA32Float,
                                          offsetof(asset::Vertex, tangent)},
                                         {3, gpu::Format::kRG32Float, offsetof(asset::Vertex, uv)},
                                         {4, gpu::Format::kRGBA8Unorm,
                                          offsetof(asset::Vertex, color)}}}},
      .raster = {.cull = gpu::CullMode::kNone},
      .depth = {.test = true,
                .write = true,
                .compare = gpu::CompareOp::kGreater,  // reversed z
                .format = depth_format},
      .color_formats = {color_format, motion_format},
      .blend = {gpu::BlendMode::kOpaque, gpu::BlendMode::kOpaque},
      .sets = {{.shared = globals_layout},
               {.shared = material_layout},
               {.shared = environment_layout},
               {.shared = bindless_layout},
               {.slots = {{0, gpu::BindingType::kCombinedTextureSampler},
                          {1, gpu::BindingType::kCombinedTextureSampler}},
                .stages = gpu::kShaderStageFragment}},
      .push_constant_size = gpu::PushSize<MeshPushConstants>(),
      .push_bda = gpu::PushBdaHeader::kMeshDraw,
      .debug_name = "water",
  });
  if (!pass->pipeline_) {
    RX_ERROR("water pipeline creation failed");
    return nullptr;
  }

  // Snapshot copy compute.
  pass->copy_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_copy_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kCombinedTextureSampler}}}},
      .debug_name = "water_copy",
  });
  if (!pass->copy_pipeline_) {
    RX_ERROR("water copy pipeline creation failed");
    return nullptr;
  }
  if (!pass->adaptive_.Initialize(device)) {
    RX_WARN("adaptive water unavailable; using authored water meshes");
  }
  return pass;
}

WaterPass::~WaterPass() {
  adaptive_.Destroy(device_);
  if (pipeline_) device_.DestroyPipeline(pipeline_);
  if (copy_pipeline_) device_.DestroyPipeline(copy_pipeline_);
}

void WaterPass::RecordCopy(PassContext& ctx, ResourceHandle scene_color,
                           ResourceHandle opaque_color, u32 width, u32 height) {
  ctx.cmd->BindPipeline(copy_pipeline_);
  ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, ctx.graph->image(opaque_color)),
                             gpu::Bind::Combined(1, ctx.graph->image(scene_color).view, sampler_)});
  ctx.cmd->Dispatch2D({width, height});
}

void WaterPass::Bind(PassContext& ctx, gpu::BindingSetHandle globals, gpu::BindingSetHandle environment,
                     gpu::BindingSetHandle bindless, ResourceHandle opaque_color,
                     ResourceHandle opaque_depth) {
  ctx.cmd->BindPipeline(pipeline_);
  ctx.cmd->BindSet(0, globals);
  ctx.cmd->BindSet(2, environment);
  ctx.cmd->BindSet(3, bindless);
  ctx.cmd->BindTransient(4, {gpu::Bind::Combined(0, ctx.graph->image(opaque_color).view, sampler_),
                             gpu::Bind::Combined(1, ctx.graph->image(opaque_depth).view, sampler_)});
}

void WaterPass::BindMaterial(gpu::CommandList& cmd, gpu::BindingSetHandle material) {
  cmd.BindSet(1, material);
}

}  // namespace rx::render
