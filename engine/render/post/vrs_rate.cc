#include "render/post/vrs_rate.h"

#include "core/log.h"
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

bool VrsRatePass::Initialize(Device& device) {
  if (!device.caps().fragment_shading_rate) return false;
  texel_size_ = device.caps().shading_rate_texel;
  // 4x4 visibly stripes glossy floors (coarse specular moires against the
  // upscaler's per-pixel reconstruction); 2x2 is the safe ceiling.
  allow_coarse_ = false;

  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vrs_rate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(RatePush),
      .debug_name = "vrs_rate",
  });
  if (!pipeline_) {
    RX_ERROR("vrs rate pipeline creation failed");
    return false;
  }
  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});
  return true;
}

void VrsRatePass::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  if (rate_) device.DestroyImage(rate_);
  rate_ = {};
}

bool VrsRatePass::Resize(Device& device, Extent2D render_extent) {
  if (!pipeline_) return false;
  if (rate_ && render_extent.width == render_extent_.width &&
      render_extent.height == render_extent_.height) {
    return true;
  }
  if (rate_) device.DestroyImage(rate_);
  render_extent_ = render_extent;
  Extent2D rate_extent{(render_extent.width + texel_size_ - 1) / texel_size_,
                       (render_extent.height + texel_size_ - 1) / texel_size_};
  rate_ = device.CreateImage2D(Format::kR8Uint, rate_extent,
                               kTextureUsageShadingRate | kTextureUsageStorage |
                                   kTextureUsageTransferDst);
  if (!rate_) {
    RX_WARN("vrs rate image unavailable");
    return false;
  }
  // Zero = 1x1 everywhere until the first rebuild, then park in the
  // attachment state the scene pass expects.
  device.ImmediateSubmit([this](CommandList& cmd) {
    TextureBarrier to_clear[1] = {
        Transition(rate_, ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    const f32 zero[4] = {0, 0, 0, 0};
    cmd.ClearColor(rate_, zero);
    TextureBarrier to_rate[1] = {
        Transition(rate_, ResourceState::kCopyDst, ResourceState::kShadingRate)};
    cmd.TextureBarriers(to_rate);
  });
  return true;
}

void VrsRatePass::AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle motion,
                             Extent2D render_extent, f32 threshold, f32 motion_scale) {
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

        TextureBarrier to_write[1] = {
            Transition(rate_, ResourceState::kShadingRate, ResourceState::kGeneral)};
        ctx.cmd->TextureBarriers(to_write);
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, rate_),
                Bind::Combined(1, ctx.graph->image(lit).view, sampler_),
                Bind::Sampled(2, ctx.graph->image(motion))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch((rate_.extent.width + 7) / 8, (rate_.extent.height + 7) / 8, 1);
        TextureBarrier to_rate[1] = {
            Transition(rate_, ResourceState::kGeneral, ResourceState::kShadingRate)};
        ctx.cmd->TextureBarriers(to_rate);
      });
}

}  // namespace rx::render
