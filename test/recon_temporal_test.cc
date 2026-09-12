#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "core/math.h"
#include "render/rhi/device.h"
#include "shaders/recon_temporal_cs_hlsl.h"

using namespace rx;
using namespace rx::render;

namespace {

struct Push {
  Mat4 prev_view_proj = Mat4::Identity();
  f32 camera_pos[4] = {};
  u32 size[2] = {3, 1};
  f32 inv_size[2] = {1.0f / 3.0f, 1.0f};
  f32 current_weight_min = 0.05f;
  f32 max_history = 32;
  f32 reset = 1;
  u32 spec_mode = 0;
};

}  // namespace

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0
                     ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    std::printf("recon_temporal_test: SKIP, GPU unavailable\n");
    return 0;
  }
  int failures = 0;
  auto check = [&](bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
  };
  ComputePipelineDesc pipeline_desc;
  pipeline_desc.shader = RX_SHADER(k_recon_temporal_cs_hlsl);
  pipeline_desc.sets.resize(1);
  for (u32 i = 0; i < 13; ++i)
    pipeline_desc.sets[0].slots.push_back(
        {i, i < 2 ? BindingType::kStorageImage : BindingType::kSampledImage});
  pipeline_desc.push_constant_size = PushSize<Push>();
  pipeline_desc.debug_name = "recon_temporal_test";
  PipelineHandle pipeline = device->CreateComputePipeline(pipeline_desc);
  if (!pipeline) return 1;
  std::vector<GpuImage> owned;
  auto input = [&](Format format, const void* data, u32 size) {
    GpuImage image = device->CreateImage2D(
        format, {3, 1}, kTextureUsageSampled | kTextureUsageTransferDst);
    GpuBuffer staging = device->CreateBufferWithData(
        {static_cast<const u8*>(data), size}, kBufferUsageTransferSrc);
    if (!image || !staging) std::exit(1);
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
      const BufferTextureCopy copy{.extent = {3, 1}};
      cmd.CopyBufferToTexture(staging, image, {&copy, 1});
      cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadCompute));
    });
    device->DestroyBuffer(staging);
    owned.push_back(image);
    return image;
  };
  const f32 noisy[12] = {0, 0, 0, 0, 1000, 1000, 1000, 0, 0, 0, 0, 0};
  const f32 normals[12] = {.5f, .5f, 1, 1, .5f, .5f, 1, 1, .5f, .5f, 1, 1};
  const f32 depth[3] = {1, 1, 1};
  const u32 material[3] = {};
  const f32 motion[6] = {};
  f32 history[12] = {};
  for (f32& v : history) v = std::numeric_limits<f32>::infinity();
  GpuImage curr = input(Format::kRGBA32Float, noisy, sizeof(noisy));
  GpuImage nr = input(Format::kRGBA32Float, normals, sizeof(normals));
  GpuImage vz = input(Format::kR32Float, depth, sizeof(depth));
  GpuImage id = input(Format::kR32Uint, material, sizeof(material));
  GpuImage mv = input(Format::kRG32Float, motion, sizeof(motion));
  GpuImage prev = input(Format::kRGBA32Float, history, sizeof(history));
  const f32 negative_history[12] = {0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1};
  GpuImage negative = input(Format::kRGBA32Float, negative_history, sizeof(negative_history));
  GpuImage accum = device->CreateImage2D(
      Format::kRGBA16Float, {3, 1}, kTextureUsageStorage | kTextureUsageTransferSrc);
  GpuImage moments = device->CreateImage2D(
      Format::kRGBA32Float, {3, 1}, kTextureUsageStorage | kTextureUsageTransferSrc);
  if (!accum || !moments) return 1;
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(accum, ResourceState::kUndefined, ResourceState::kGeneral));
    cmd.Barrier(Transition(moments, ResourceState::kUndefined, ResourceState::kGeneral));
  });
  for (u32 frame = 0; frame < 3; ++frame) {
    Push push;
    push.reset = frame == 0 ? 1.0f : 0.0f;
    if (frame == 2) {
      push.max_history = 0;
      push.current_weight_min = 2;
    }
    device->ImmediateSubmit([&](CommandList& cmd) {
      if (frame > 0) {
        cmd.Barrier(Transition(accum, ResourceState::kCopySrc, ResourceState::kGeneral));
        cmd.Barrier(Transition(moments, ResourceState::kCopySrc, ResourceState::kGeneral));
      }
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {Bind::Storage(0, accum), Bind::Storage(1, moments),
                            Bind::Sampled(2, curr), Bind::Sampled(3, frame == 2 ? curr : prev),
                            Bind::Sampled(4, nr), Bind::Sampled(5, nr),
                            Bind::Sampled(6, vz), Bind::Sampled(7, vz),
                            Bind::Sampled(8, mv), Bind::Sampled(9, id),
                            Bind::Sampled(10, id), Bind::Sampled(11, frame == 2 ? negative : prev),
                            Bind::Sampled(12, curr)});
      cmd.Push(push);
      cmd.Dispatch(1, 1, 1);
    });
    f32 values[12]{};
    check(device->ReadbackImage(moments, ResourceState::kGeneral, values, sizeof(values)),
          "read back temporal moments");
    for (f32 value : values) check(std::isfinite(value), "HDR moments must remain finite");
    check(std::abs(values[5] - 1000000.0f) < 1.0f, "squared HDR luminance must not overflow");
    check(values[7] == 1.0f, "reset or invalid history must seed one frame");
    u16 half[12]{};
    check(device->ReadbackImage(accum, ResourceState::kGeneral, half, sizeof(half)),
          "read back accumulated radiance");
    for (u16 value : half) check((value & 0x7c00u) != 0x7c00u, "half output must remain finite");
  }
  device->WaitIdle();
  for (GpuImage& image : owned) device->DestroyImage(image);
  device->DestroyImage(accum);
  device->DestroyImage(moments);
  device->DestroyPipeline(pipeline);
  std::printf("recon_temporal_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
