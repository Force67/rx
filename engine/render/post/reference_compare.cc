#include "render/post/reference_compare.h"

#include <cmath>
#include <cstring>

#include <stb_image.h>

#include "core/log.h"
#include "shaders/reference_compare_cs_hlsl.h"

namespace rx::render {
namespace {

struct ComparePush {
  u32 size[2];
  f32 inv_size[2];
  f32 ref_uv_scale[2];
  f32 ref_uv_offset[2];
  f32 ref_exposure;
  f32 difference_gain;
  f32 split;
  u32 mode;
  u32 tonemap_op;
  u32 region;
  u32 stats;
  f32 exposure_scale;
};

constexpr u32 kStatSlots = 16;  // 4 regions x 4 accumulators
constexpr f64 kStatScale = 65536.0;

}  // namespace

bool ReferenceCompare::Initialize(Device& device) {
  device_ = &device;
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_reference_compare_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = PushSize<ComparePush>(),
      .debug_name = "reference_compare",
  });
  if (!pipeline_) return false;
  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});
  // Host-visible so the fitting loop can read the metric without a staging
  // round trip; it is 64 bytes.
  stats_buffer_ = device.CreateBuffer(kStatSlots * sizeof(u32), kBufferUsageStorage, true);
  if (!stats_buffer_.mapped) return false;
  std::memset(stats_buffer_.mapped, 0, kStatSlots * sizeof(u32));

  // "No mask loaded" must mean "every region is everywhere", not "nothing is
  // anything" - otherwise turning stats on before authoring a mask silently
  // reports zero error.
  white_mask_ = device.CreateImage2D(Format::kRGBA8Unorm, {1, 1},
                                     kTextureUsageSampled | kTextureUsageTransferDst);
  if (white_mask_) {
    const u8 px[4] = {255, 255, 255, 255};
    GpuBuffer staging = device.CreateBuffer(4, kBufferUsageTransferSrc, true);
    std::memcpy(staging.mapped, px, 4);
    device.ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(white_mask_, ResourceState::kUndefined, ResourceState::kCopyDst));
      BufferTextureCopy region{};
      cmd.CopyBufferToTexture(staging, white_mask_, {&region, 1});
      cmd.Barrier(Transition(white_mask_, ResourceState::kCopyDst,
                             ResourceState::kShaderReadFragment));
    });
    device.DestroyBuffer(staging);
  }
  return true;
}

void ReferenceCompare::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  device.DestroyImage(reference_);
  device.DestroyImage(region_mask_);
  device.DestroyImage(white_mask_);
  device.DestroyBuffer(stats_buffer_);
  reference_ = {};
  region_mask_ = {};
  white_mask_ = {};
  device_ = nullptr;
}

namespace {

// Uploads float rgba into a fresh RGBA32F image. Reference frames are the one
// place in the engine where full float is worth the memory: a 16-bit reference
// quantizes exactly the shadow detail the terminator fit lives in.
GpuImage UploadFloatImage(Device& device, const f32* rgba, u32 width, u32 height) {
  GpuImage image = device.CreateImage2D(Format::kRGBA32Float, {width, height},
                                        kTextureUsageSampled | kTextureUsageTransferDst);
  if (!image) return {};
  const u64 bytes = static_cast<u64>(width) * height * 4 * sizeof(f32);
  GpuBuffer staging = device.CreateBuffer(bytes, kBufferUsageTransferSrc, true);
  if (!staging.mapped) {
    device.DestroyImage(image);
    return {};
  }
  std::memcpy(staging.mapped, rgba, bytes);
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
    BufferTextureCopy region{};
    cmd.CopyBufferToTexture(staging, image, {&region, 1});
    cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadFragment));
  });
  device.DestroyBuffer(staging);
  return image;
}

}  // namespace

bool ReferenceCompare::LoadReference(Device& device, const std::string& path) {
  int w = 0, h = 0, comp = 0;
  // stbi_loadf returns scene-linear for .hdr and de-gammas 8-bit sources on the
  // way in, which is the contract this pass needs: everything downstream of
  // here is scene-linear.
  f32* pixels = stbi_loadf(path.c_str(), &w, &h, &comp, 4);
  if (!pixels) {
    RX_WARN("reference compare: cannot read {} ({})", path, stbi_failure_reason());
    return false;
  }
  GpuImage image = UploadFloatImage(device, pixels, static_cast<u32>(w), static_cast<u32>(h));
  stbi_image_free(pixels);
  if (!image) return false;
  device.DestroyImageDeferred(reference_);
  reference_ = image;
  RX_INFO("reference compare: loaded {} ({}x{})", path, w, h);
  return true;
}

bool ReferenceCompare::LoadRegionMask(Device& device, const std::string& path) {
  int w = 0, h = 0, comp = 0;
  f32* pixels = stbi_loadf(path.c_str(), &w, &h, &comp, 4);
  if (!pixels) {
    RX_WARN("reference compare: cannot read region mask {}", path);
    return false;
  }
  GpuImage image = UploadFloatImage(device, pixels, static_cast<u32>(w), static_cast<u32>(h));
  stbi_image_free(pixels);
  if (!image) return false;
  device.DestroyImageDeferred(region_mask_);
  region_mask_ = image;
  return true;
}

ResourceHandle ReferenceCompare::AddToGraph(RenderGraph& graph, ResourceHandle scene_color,
                                            Extent2D extent, u32 tonemap_op) {
  if (!pipeline_ || settings_.mode == Mode::kOff || !reference_) return scene_color;

  ResourceHandle out = graph.CreateTexture({.name = "reference_compare",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  Settings s = settings_;
  graph.AddPass(
      "reference_compare",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(scene_color, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, scene_color, out, extent, s, tonemap_op](PassContext& ctx) {
        // The metric accumulates for exactly one frame, so a fitting step reads
        // one frame's error and not a running sum of every frame it displayed.
        if (s.collect_stats) std::memset(stats_buffer_.mapped, 0, kStatSlots * sizeof(u32));
        ComparePush push{};
        push.size[0] = extent.width;
        push.size[1] = extent.height;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent.height);
        push.ref_uv_scale[0] = s.uv_scale[0];
        push.ref_uv_scale[1] = s.uv_scale[1];
        push.ref_uv_offset[0] = s.uv_offset[0];
        push.ref_uv_offset[1] = s.uv_offset[1];
        push.ref_exposure = s.reference_exposure;
        push.difference_gain = s.difference_gain;
        push.split = s.split;
        push.mode = static_cast<u32>(s.mode);
        push.tonemap_op = tonemap_op;
        push.region = static_cast<u32>(s.region);
        push.stats = s.collect_stats ? 1u : 0u;
        push.exposure_scale = s.exposure_scale;

        const GpuImage& mask = region_mask_ ? region_mask_ : white_mask_;
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out)),
                Bind::Combined(1, reference_.view, sampler_),
                Bind::Combined(2, mask.view, sampler_),
                Bind::StorageBuffer(3, stats_buffer_),
                Bind::Combined(4, ctx.graph->image(scene_color).view, sampler_)});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });
  return out;
}

ReferenceCompare::Stats ReferenceCompare::stats(Region region) const {
  Stats out;
  if (!stats_buffer_.mapped) return out;
  const u32 index = region == Region::kAll ? 0u : static_cast<u32>(region) - 1u;
  const u32* raw = static_cast<const u32*>(stats_buffer_.mapped) + index * 4;
  const f64 count = static_cast<f64>(raw[3]) / kStatScale;
  if (count <= 0.0) return out;
  out.coverage = count;
  out.mean_squared_error = static_cast<f64>(raw[0]) / kStatScale / count;
  out.mean_absolute_error = static_cast<f64>(raw[1]) / kStatScale / count / 3.0;
  out.mean_reference_luma = static_cast<f64>(raw[2]) / kStatScale / count;
  return out;
}

}  // namespace rx::render
