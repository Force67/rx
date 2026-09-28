#include "rxe/render/post/vrs_rate.h"

#include "foundation/logging/log.h"
#include "shaders/vrs_rate_cs_hlsl.h"

namespace rx::render {
namespace {

struct RatePush {
  u32 render_size[2];
  u32 rate_size[2];
  u32 texel_size;
  f32 threshold;
  f32 motion_scale;
  u32 allow_coarse;
};

}  // namespace

bool VrsRatePass::Initialize(gpu::Device& device) {
  if (!device.caps().fragment_shading_rate) return false;
  texel_size_ = device.caps().shading_rate_texel;
  // 4x4 visibly stripes glossy floors (coarse specular moires against the
  // upscaler's per-pixel reconstruction); 2x2 is the safe ceiling.
  allow_coarse_ = false;

  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vrs_rate_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kCombinedTextureSampler},
                          {2, gpu::BindingType::kSampledImage}}}},
      .push_constant_size = gpu::PushSize<RatePush>(),
      .debug_name = "vrs_rate",
  });
  if (!pipeline_) {
    RX_ERROR("vrs rate pipeline creation failed");
    return false;
  }
  sampler_ = device.GetSampler({.min_filter = gpu::Filter::kLinear,
                                .mag_filter = gpu::Filter::kLinear,
                                .address_u = gpu::AddressMode::kClampToEdge,
                                .address_v = gpu::AddressMode::kClampToEdge});
  return true;
}

void VrsRatePass::Destroy(gpu::Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  if (rate_) device.DestroyImage(rate_);
  rate_ = {};
}

bool VrsRatePass::Resize(gpu::Device& device, gpu::Extent2D render_extent) {
  if (!pipeline_) return false;
  if (rate_ && render_extent.width == render_extent_.width &&
      render_extent.height == render_extent_.height) {
    return true;
  }
  if (rate_) device.DestroyImage(rate_);
  render_extent_ = render_extent;
  gpu::Extent2D rate_extent{(render_extent.width + texel_size_ - 1) / texel_size_,
                       (render_extent.height + texel_size_ - 1) / texel_size_};
  rate_ = device.CreateImage2D(gpu::Format::kR8Uint, rate_extent,
                               gpu::kTextureUsageShadingRate | gpu::kTextureUsageStorage |
                                   gpu::kTextureUsageTransferDst);
  if (!rate_) {
    RX_WARN("vrs rate image unavailable");
    return false;
  }
  // Zero = 1x1 everywhere until the first rebuild, then park in the
  // attachment state the scene pass expects.
  device.ImmediateSubmit([this](gpu::CommandList& cmd) {
    gpu::TextureBarrier to_clear[1] = {
        gpu::Transition(rate_, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    const f32 zero[4] = {0, 0, 0, 0};
    cmd.ClearColor(rate_, zero);
    gpu::TextureBarrier to_rate[1] = {
        gpu::Transition(rate_, gpu::ResourceState::kCopyDst, gpu::ResourceState::kShadingRate)};
    cmd.TextureBarriers(to_rate);
  });
  return true;
}

void VrsRatePass::AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle motion,
                             gpu::Extent2D render_extent, f32 threshold, f32 motion_scale) {
  if (!available()) return;
  graph.AddPass(
      "vrs_rate",
      [&](RenderGraph::PassBuilder& b) {
        b.Read(lit, ResourceUsage::kSampledCompute);
        b.Read(motion, ResourceUsage::kSampledCompute);
      },
      [this, lit, motion, render_extent, threshold, motion_scale](PassContext& ctx) {
        RatePush push{};
        push.render_size[0] = render_extent.width;
        push.render_size[1] = render_extent.height;
        push.rate_size[0] = rate_.extent.width;
        push.rate_size[1] = rate_.extent.height;
        push.texel_size = texel_size_;
        push.threshold = threshold;
        push.motion_scale = motion_scale;
        push.allow_coarse = allow_coarse_ ? 1u : 0u;

        gpu::TextureBarrier to_write[1] = {
            gpu::Transition(rate_, gpu::ResourceState::kShadingRate, gpu::ResourceState::kGeneral)};
        ctx.cmd->TextureBarriers(to_write);
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {gpu::Bind::Storage(0, rate_),
                gpu::Bind::Combined(1, ctx.graph->image(lit).view, sampler_),
                gpu::Bind::Sampled(2, ctx.graph->image(motion))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch((rate_.extent.width + 7) / 8, (rate_.extent.height + 7) / 8, 1);
        gpu::TextureBarrier to_rate[1] = {
            gpu::Transition(rate_, gpu::ResourceState::kGeneral, gpu::ResourceState::kShadingRate)};
        ctx.cmd->TextureBarriers(to_rate);
      });
}

}  // namespace rx::render
