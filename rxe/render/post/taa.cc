#include "rxe/render/post/antialiasing.h"

#include "foundation/logging/log.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/taa_cs_hlsl.h"

namespace rx::render {
namespace {

constexpr gpu::Format kHistoryFormat = gpu::Format::kRGBA16Float;

struct TaaPushConstants {
  f32 inv_size[2];
  f32 history_blend;
  u32 reset_history;
  u32 debug;  // 1 = output the disocclusion heatmap instead of the resolved color
  u32 pad[3];
};

f32 Halton(u32 index, u32 base) {
  f32 result = 0.0f;
  f32 fraction = 1.0f;
  for (u32 i = index + 1; i > 0; i /= base) {
    fraction /= static_cast<f32>(base);
    result += fraction * static_cast<f32>(i % base);
  }
  return result;
}

}  // namespace

void JitterSequence::Sample(u32 frame_index, u32 sample_count, f32* out_x, f32* out_y) {
  u32 index = frame_index % sample_count;
  *out_x = Halton(index, 2) - 0.5f;
  *out_y = Halton(index, 3) - 0.5f;
}

bool TaaPass::Initialize(gpu::Device& device) {
  sampler_ = device.GetSampler({.address_u = gpu::AddressMode::kClampToEdge,
                                .address_v = gpu::AddressMode::kClampToEdge,
                                .address_w = gpu::AddressMode::kClampToEdge});

  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_taa_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kCombinedTextureSampler},
                          {2, gpu::BindingType::kCombinedTextureSampler},
                          {3, gpu::BindingType::kCombinedTextureSampler},
                          {4, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<TaaPushConstants>(),
      .debug_name = "taa",
  });
  if (!pipeline_) {
    RX_ERROR("taa pipeline creation failed");
    return false;
  }
  return true;
}

void TaaPass::Resize(gpu::Device& device, gpu::Extent2D extent) {
  for (u32 i = 0; i < 2; ++i) {
    if (history_[i]) device.DestroyImage(history_[i]);
    history_[i] = device.CreateImage2D(kHistoryFormat, extent,
                                       gpu::kTextureUsageSampled | gpu::kTextureUsageStorage);
    history_states_[i] = gpu::ResourceState::kUndefined;
  }
  extent_ = extent;
  history_valid_ = false;
}

void TaaPass::Destroy(gpu::Device& device) {
  for (gpu::GpuImage& image : history_) {
    if (image) device.DestroyImage(image);
  }
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  sampler_ = {};
}

ResourceHandle TaaPass::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                   ResourceHandle motion, u32 frame_index, u32 debug_mode) {
  bool debug_disocclusion = debug_mode != 0;
  u32 write_index = frame_index % 2;
  u32 read_index = 1 - write_index;
  ResourceHandle resolved =
      graph.ImportImage("taa_resolved", history_[write_index], &history_states_[write_index]);
  ResourceHandle history =
      graph.ImportImage("taa_history", history_[read_index], &history_states_[read_index]);

  bool reset = !history_valid_;
  history_valid_ = true;

  // The disocclusion view writes its heatmap to a side target so the resolved
  // history keeps accumulating the real color (writing the heatmap back into the
  // ping-pong would feed it in as history and read as full rejection forever).
  ResourceHandle debug_target = kInvalidResource;
  if (debug_disocclusion) {
    debug_target = graph.CreateTexture({.name = "taa_disocclusion",
                                        .format = gpu::Format::kRGBA16Float,
                                        .width = extent_.width,
                                        .height = extent_.height});
  }

  graph.AddPass(
      "taa_resolve",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(color, ResourceUsage::kSampledCompute);
        builder.Read(motion, ResourceUsage::kSampledCompute);
        builder.Read(history, ResourceUsage::kSampledCompute);
        builder.Write(resolved, ResourceUsage::kStorageWrite);
        if (debug_target != kInvalidResource) builder.Write(debug_target, ResourceUsage::kStorageWrite);
      },
      [this, color, motion, history, resolved, reset, debug_mode,
       debug_target](PassContext& ctx) {
        gpu::TextureView debug_view =
            debug_target != kInvalidResource ? ctx.graph->image(debug_target).view
                                             : ctx.graph->image(resolved).view;

        TaaPushConstants push{};
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.history_blend = settings_.history_blend;
        push.reset_history = reset ? 1u : 0u;
        push.debug = debug_mode;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Storage(0, ctx.graph->image(resolved)),
                gpu::Bind::Combined(1, ctx.graph->image(color).view, sampler_),
                gpu::Bind::Combined(2, ctx.graph->image(history).view, sampler_),
                gpu::Bind::Combined(3, ctx.graph->image(motion).view, sampler_),
                gpu::Bind::StorageView(4, debug_view)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
  return debug_target != kInvalidResource ? debug_target : resolved;
}

}  // namespace rx::render
