#include "render/post/depth_of_field.h"

#include "core/log.h"
#include "shaders/dof_coc_cs_hlsl.h"
#include "shaders/dof_composite_cs_hlsl.h"
#include "shaders/dof_focus_cs_hlsl.h"
#include "shaders/dof_gather_cs_hlsl.h"

namespace rx::render {
namespace {

struct FocusPush {
  f32 near_plane;
  f32 focus_speed;
  f32 focus_override;
  u32 reset;
};
struct CocPush {
  u32 size[2];
  f32 near_plane;
  f32 aperture;
  f32 max_coc;
  f32 pad[3];
};
struct GatherPush {
  u32 size[2];
  f32 inv_size[2];
  f32 max_coc;
  f32 pad[3];
};
struct CompositePush {
  u32 size[2];
  f32 inv_size[2];
};

}  // namespace

bool DepthOfFieldPass::Initialize(Device& device) {
  focus_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_dof_focus_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage}}}},
      .push_constant_size = PushSize<FocusPush>(),
      .debug_name = "dof_focus",
  });
  coc_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_dof_coc_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage}}}},
      .push_constant_size = PushSize<CocPush>(),
      .debug_name = "dof_coc",
  });
  gather_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_dof_gather_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = PushSize<GatherPush>(),
      .debug_name = "dof_gather",
  });
  composite_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_dof_composite_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = PushSize<CompositePush>(),
      .debug_name = "dof_composite",
  });
  if (!focus_pipeline_ || !coc_pipeline_ || !gather_pipeline_ || !composite_pipeline_) {
    RX_ERROR("dof pipeline creation failed");
    return false;
  }
  focus_state_ = device.CreateImage2D(Format::kR32Float, {1, 1},
                                     kTextureUsageStorage | kTextureUsageSampled);
  focus_layout_ = ResourceState::kUndefined;
  focus_valid_ = false;
  if (!focus_state_) return false;
  sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});
  return true;
}

void DepthOfFieldPass::Destroy(Device& device) {
  for (PipelineHandle* p : {&focus_pipeline_, &coc_pipeline_, &gather_pipeline_, &composite_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  if (focus_state_) device.DestroyImage(focus_state_);
  focus_state_ = {};
  focus_valid_ = false;
}

ResourceHandle DepthOfFieldPass::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                            ResourceHandle depth, Extent2D extent,
                                            const Frame& frame) {
  if (!focus_state_ || !focus_pipeline_ || !coc_pipeline_ || !gather_pipeline_ ||
      !composite_pipeline_) return color;
  ResourceHandle focus = graph.ImportImage("dof_focus", focus_state_, &focus_layout_);
  const bool reset = !focus_valid_;
  focus_valid_ = true;
  Extent2D half{(extent.width + 1) / 2, (extent.height + 1) / 2};
  ResourceHandle coc = graph.CreateTexture(
      {.name = "dof_coc", .format = Format::kR16Float, .width = extent.width,
       .height = extent.height});
  ResourceHandle gathered = graph.CreateTexture(
      {.name = "dof_gather", .format = Format::kRGBA16Float, .width = half.width,
       .height = half.height});
  ResourceHandle out = graph.CreateTexture(
      {.name = "dof_out", .format = Format::kRGBA16Float, .width = extent.width,
       .height = extent.height});

  graph.AddPass(
      "dof_focus",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(focus, ResourceUsage::kStorageWrite);
        b.Read(depth, ResourceUsage::kSampledCompute);
      },
      [this, focus, depth, frame, reset](PassContext& ctx) {
        FocusPush p{frame.near_plane, frame.focus_speed, frame.focus_distance, reset ? 1u : 0u};
        ctx.cmd->BindPipeline(focus_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(focus)),
                                   Bind::Sampled(1, ctx.graph->image(depth))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch(1, 1, 1);
      });

  graph.AddPass(
      "dof_coc",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(coc, ResourceUsage::kStorageWrite);
        b.Read(depth, ResourceUsage::kSampledCompute);
        b.Read(focus, ResourceUsage::kSampledCompute);
      },
      [this, coc, depth, focus, extent, frame](PassContext& ctx) {
        CocPush p{};
        p.size[0] = extent.width;
        p.size[1] = extent.height;
        p.near_plane = frame.near_plane;
        p.aperture = frame.aperture;
        p.max_coc = frame.max_coc;
        ctx.cmd->BindPipeline(coc_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(coc)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::Sampled(2, ctx.graph->image(focus))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });

  graph.AddPass(
      "dof_gather",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(gathered, ResourceUsage::kStorageWrite);
        b.Read(color, ResourceUsage::kSampledCompute);
        b.Read(coc, ResourceUsage::kSampledCompute);
      },
      [this, gathered, color, coc, half, frame](PassContext& ctx) {
        GatherPush p{};
        p.size[0] = half.width;
        p.size[1] = half.height;
        p.inv_size[0] = 1.0f / static_cast<f32>(half.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(half.height);
        p.max_coc = frame.max_coc;
        ctx.cmd->BindPipeline(gather_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(gathered)),
                Bind::Combined(1, ctx.graph->image(color).view, sampler_),
                Bind::Combined(2, ctx.graph->image(coc).view, sampler_)});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(half);
      });

  graph.AddPass(
      "dof_composite",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {color, gathered, coc})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, out, color, gathered, coc, extent](PassContext& ctx) {
        CompositePush p{};
        p.size[0] = extent.width;
        p.size[1] = extent.height;
        p.inv_size[0] = 1.0f / static_cast<f32>(extent.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(extent.height);
        ctx.cmd->BindPipeline(composite_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out)),
                Bind::Sampled(1, ctx.graph->image(color)),
                Bind::Combined(2, ctx.graph->image(gathered).view, sampler_),
                Bind::Sampled(3, ctx.graph->image(coc))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rx::render
