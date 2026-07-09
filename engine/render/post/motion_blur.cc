#include "render/post/motion_blur.h"

#include "core/log.h"
#include "shaders/motion_blur_cs_hlsl.h"
#include "shaders/motion_tilemax_cs_hlsl.h"

namespace rx::render {
namespace {

constexpr u32 kTileSize = 16;

struct TileMaxPush {
  u32 tile_count[2];
  u32 size[2];
  f32 scale[2];
  f32 max_blur[2];
  f32 debug_vel[2];
  f32 pad[2];
};

struct BlurPush {
  u32 size[2];
  f32 inv_size[2];
  u32 tile_count[2];
  f32 vel_scale[2];
  f32 max_blur[2];
  u32 samples;
  u32 frame_index;
  f32 debug_vel[2];
};

}  // namespace

bool MotionBlurPass::Initialize(Device& device) {
  tilemax_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_motion_tilemax_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(TileMaxPush),
      .debug_name = "motion_tilemax",
  });
  blur_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_motion_blur_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(BlurPush),
      .debug_name = "motion_blur",
  });
  if (!tilemax_pipeline_ || !blur_pipeline_) {
    RX_ERROR("motion blur pipeline creation failed");
    return false;
  }
  sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});
  return true;
}

void MotionBlurPass::Destroy(Device& device) {
  for (PipelineHandle* p : {&tilemax_pipeline_, &blur_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
}

ResourceHandle MotionBlurPass::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                          ResourceHandle motion, Extent2D extent,
                                          const Frame& frame) {
  u32 tiles_x = (extent.width + kTileSize - 1) / kTileSize;
  u32 tiles_y = (extent.height + kTileSize - 1) / kTileSize;
  ResourceHandle tiles = graph.CreateTexture({.name = "mb_tilemax",
                                              .format = Format::kRG16Float,
                                              .width = tiles_x,
                                              .height = tiles_y});
  ResourceHandle out = graph.CreateTexture({.name = "mb_color",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});

  f32 max_blur_uv[2] = {frame.max_blur_px / static_cast<f32>(extent.width),
                        frame.max_blur_px / static_cast<f32>(extent.height)};
  f32 debug_uv[2] = {frame.debug_velocity[0] / static_cast<f32>(extent.width),
                     frame.debug_velocity[1] / static_cast<f32>(extent.height)};

  graph.AddPass(
      "mb_tilemax",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(tiles, ResourceUsage::kStorageWrite);
        b.Read(motion, ResourceUsage::kSampledCompute);
      },
      [this, tiles, motion, tiles_x, tiles_y, max_blur_uv, debug_uv, frame](PassContext& ctx) {
        const GpuImage& mv = ctx.graph->image(motion);
        TileMaxPush p{};
        p.tile_count[0] = tiles_x;
        p.tile_count[1] = tiles_y;
        p.size[0] = mv.extent.width;
        p.size[1] = mv.extent.height;
        p.scale[0] = p.scale[1] = frame.shutter;
        p.max_blur[0] = max_blur_uv[0];
        p.max_blur[1] = max_blur_uv[1];
        p.debug_vel[0] = debug_uv[0];
        p.debug_vel[1] = debug_uv[1];
        ctx.cmd->BindPipeline(tilemax_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(tiles)),
                                   Bind::Sampled(1, mv)});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D({tiles_x, tiles_y});
      });

  graph.AddPass(
      "motion_blur",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out, ResourceUsage::kStorageWrite);
        for (ResourceHandle h : {color, motion, tiles})
          b.Read(h, ResourceUsage::kSampledCompute);
      },
      [this, out, color, motion, tiles, extent, tiles_x, tiles_y, max_blur_uv, debug_uv,
       frame](PassContext& ctx) {
        BlurPush p{};
        p.size[0] = extent.width;
        p.size[1] = extent.height;
        p.inv_size[0] = 1.0f / static_cast<f32>(extent.width);
        p.inv_size[1] = 1.0f / static_cast<f32>(extent.height);
        p.tile_count[0] = tiles_x;
        p.tile_count[1] = tiles_y;
        p.vel_scale[0] = p.vel_scale[1] = frame.shutter;
        p.max_blur[0] = max_blur_uv[0];
        p.max_blur[1] = max_blur_uv[1];
        p.samples = frame.samples;
        p.frame_index = frame.frame_index % 64u;
        p.debug_vel[0] = debug_uv[0];
        p.debug_vel[1] = debug_uv[1];
        ctx.cmd->BindPipeline(blur_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out)),
                Bind::Combined(1, ctx.graph->image(color).view, sampler_),
                Bind::Combined(2, ctx.graph->image(motion).view, sampler_),
                Bind::Sampled(3, ctx.graph->image(tiles))});
        ctx.cmd->Push(p);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

}  // namespace rx::render
