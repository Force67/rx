#include "render/post/overdraw.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "render/gi/shadow.h"
#include "shaders/overdraw_ps_hlsl.h"
#include "shaders/shadow_instance_vs_hlsl.h"
#include "shaders/shadow_vs_hlsl.h"

namespace rx::render {

bool OverdrawPass::Initialize(Device& device, Format color_format) {
  // This pass borrows shadow.vs wholesale, so it has to borrow its interface
  // too: the same push block (the caller pushes view_proj where the cascade
  // matrix goes) and the same per-draw transform arena at set 1.
  // shadow.vs reads position (0) and uv (3); supply both from the vertex buffer.
  // TODO(rhi): blend preset mismatch: old alpha factors were srcAlpha=ZERO,
  // dstAlpha=ONE (keep dst alpha); kAdditive is ONE/ONE on alpha too. Color
  // factors (ONE/ONE) match; the heatmap only reads rgb.
  VertexBufferLayout position_stream{
      .stride = sizeof(asset::Vertex),
      .attributes = {{.location = 0,
                      .format = Format::kRGB32Float,
                      .offset = offsetof(asset::Vertex, position)},
                     {.location = 3,
                      .format = Format::kRG32Float,
                      .offset = offsetof(asset::Vertex, uv)}}};
  GraphicsPipelineDesc desc{
      .vertex = RX_SHADER(k_shadow_vs_hlsl),
      .fragment = RX_SHADER(k_overdraw_ps_hlsl),
      .vertex_buffers = {position_stream},
      .raster = {.cull = CullMode::kNone},  // count every overlapping layer
      .depth = {},                          // no test/write, no depth attachment
      .color_formats = {color_format},
      .blend = {BlendMode::kAdditive},  // additive accumulation
      // Set 0 stays empty (no alpha test here); set 1 has to be the arena,
      // because that is where shadow.vs declares it.
      .sets = {{}, {.slots = {{0, BindingType::kStorageBuffer}},
                    .stages = kShaderStageVertex}},
      .push_constant_size = PushSize<ShadowPass::Push>(),
      .push_bda = PushBdaHeader::kBones,
      .debug_name = "overdraw",
  };
  pipeline_ = device.CreateGraphicsPipeline(desc);

  VertexBufferLayout instance_stream{.stride = sizeof(Mat4),
                                     .per_instance = true,
                                     .attributes = {{7, Format::kRGBA32Float, 0},
                                                    {8, Format::kRGBA32Float, 16},
                                                    {9, Format::kRGBA32Float, 32},
                                                    {10, Format::kRGBA32Float, 48}}};
  desc.vertex = RX_SHADER(k_shadow_instance_vs_hlsl);
  desc.vertex_buffers = {position_stream, instance_stream};
  desc.debug_name = "overdraw_instanced";
  instanced_pipeline_ = device.CreateGraphicsPipeline(desc);

  if (!pipeline_ || !instanced_pipeline_) {
    RX_ERROR("overdraw pipeline creation failed");
    if (pipeline_) device.DestroyPipeline(pipeline_);
    if (instanced_pipeline_) device.DestroyPipeline(instanced_pipeline_);
    pipeline_ = {};
    instanced_pipeline_ = {};
    return false;
  }
  return true;
}

void OverdrawPass::Render(CommandList& cmd, TextureView color_view, Extent2D extent,
                          const Mat4& view_proj,
                          const std::function<void(CommandList&)>& draw) {
  ColorAttachment color{.view = color_view,
                        .load = LoadOp::kClear,  // start from black, then accumulate
                        .store = StoreOp::kStore,
                        .clear = {0.0f, 0.0f, 0.0f, 1.0f}};
  cmd.BeginRendering({.extent = extent, .colors = {&color, 1}});
  cmd.BindPipeline(pipeline_);
  cmd.PushConstants(&view_proj, sizeof(Mat4), ShadowPass::kLightMatrixOffset);
  draw(cmd);
  cmd.EndRendering();
}

void OverdrawPass::BindInstanced(CommandList& cmd, const Mat4& view_proj) {
  cmd.BindPipeline(instanced_pipeline_);
  cmd.PushConstants(&view_proj, sizeof(Mat4), ShadowPass::kLightMatrixOffset);
}

void OverdrawPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  device.DestroyPipeline(instanced_pipeline_);
  pipeline_ = {};
  instanced_pipeline_ = {};
}

}  // namespace rx::render
