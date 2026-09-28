#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/containers/vector.h"
#include "foundation/math/math.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/recon_temporal_cs_hlsl.h"

using namespace rx;
namespace gpu = rx::gpu;

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
  gpu::DeviceDesc desc;
  const char* backend = ::getenv("RX_RHI");
  desc.backend = backend && ::strcmp(backend, "d3d12") == 0
                     ? gpu::Backend::kD3D12 : gpu::Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = gpu::Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    ::printf("recon_temporal_test: SKIP, GPU unavailable\n");
    return 77;
  }
  int failures = 0;
  auto check = [&](bool ok, const char* message) {
    if (!ok) { ::printf("FAIL: %s\n", message); ++failures; }
  };
  gpu::ComputePipelineDesc pipeline_desc;
  pipeline_desc.shader = RX_SHADER(k_recon_temporal_cs_hlsl);
  pipeline_desc.sets.resize(1);
  for (u32 i = 0; i < 13; ++i)
    pipeline_desc.sets[0].slots.push_back(
        {i, i < 2 ? gpu::BindingType::kStorageImage : gpu::BindingType::kSampledImage});
  pipeline_desc.push_constant_size = gpu::PushSize<Push>();
  pipeline_desc.debug_name = "recon_temporal_test";
  gpu::PipelineHandle pipeline = device->CreateComputePipeline(pipeline_desc);
  if (!pipeline) return 1;
  base::Vector<gpu::GpuImage> owned;
  auto input = [&](gpu::Format format, const void* data, u32 size) {
    gpu::GpuImage image = device->CreateImage2D(
        format, {3, 1}, gpu::kTextureUsageSampled | gpu::kTextureUsageTransferDst);
    gpu::GpuBuffer staging = device->CreateBufferWithData(
        ByteSpan(static_cast<const u8*>(data), size), gpu::kBufferUsageTransferSrc);
    if (!image || !staging) ::exit(1);
    device->ImmediateSubmit([&](gpu::CommandList& cmd) {
      cmd.Barrier(gpu::Transition(image, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst));
      const gpu::BufferTextureCopy copy{.extent = {3, 1}};
      cmd.CopyBufferToTexture(staging, image, base::Span(&copy, 1));
      cmd.Barrier(gpu::Transition(image, gpu::ResourceState::kCopyDst, gpu::ResourceState::kShaderReadCompute));
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
  for (f32& v : history) v = INFINITY;
  gpu::GpuImage curr = input(gpu::Format::kRGBA32Float, noisy, sizeof(noisy));
  gpu::GpuImage nr = input(gpu::Format::kRGBA32Float, normals, sizeof(normals));
  gpu::GpuImage vz = input(gpu::Format::kR32Float, depth, sizeof(depth));
  gpu::GpuImage id = input(gpu::Format::kR32Uint, material, sizeof(material));
  gpu::GpuImage mv = input(gpu::Format::kRG32Float, motion, sizeof(motion));
  gpu::GpuImage prev = input(gpu::Format::kRGBA32Float, history, sizeof(history));
  const f32 negative_history[12] = {0, 0, 0, -1, 0, 0, 0, -1, 0, 0, 0, -1};
  gpu::GpuImage negative = input(gpu::Format::kRGBA32Float, negative_history, sizeof(negative_history));
  gpu::GpuImage accum = device->CreateImage2D(
      gpu::Format::kRGBA16Float, {3, 1}, gpu::kTextureUsageStorage | gpu::kTextureUsageTransferSrc);
  gpu::GpuImage moments = device->CreateImage2D(
      gpu::Format::kRGBA32Float, {3, 1}, gpu::kTextureUsageStorage | gpu::kTextureUsageTransferSrc);
  if (!accum || !moments) return 1;
  device->ImmediateSubmit([&](gpu::CommandList& cmd) {
    cmd.Barrier(gpu::Transition(accum, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral));
    cmd.Barrier(gpu::Transition(moments, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral));
  });
  for (u32 frame = 0; frame < 3; ++frame) {
    Push push;
    push.reset = frame == 0 ? 1.0f : 0.0f;
    if (frame == 2) {
      push.max_history = 0;
      push.current_weight_min = 2;
    }
    device->ImmediateSubmit([&](gpu::CommandList& cmd) {
      if (frame > 0) {
        cmd.Barrier(gpu::Transition(accum, gpu::ResourceState::kCopySrc, gpu::ResourceState::kGeneral));
        cmd.Barrier(gpu::Transition(moments, gpu::ResourceState::kCopySrc, gpu::ResourceState::kGeneral));
      }
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {gpu::Bind::Storage(0, accum), gpu::Bind::Storage(1, moments),
                            gpu::Bind::Sampled(2, curr), gpu::Bind::Sampled(3, frame == 2 ? curr : prev),
                            gpu::Bind::Sampled(4, nr), gpu::Bind::Sampled(5, nr),
                            gpu::Bind::Sampled(6, vz), gpu::Bind::Sampled(7, vz),
                            gpu::Bind::Sampled(8, mv), gpu::Bind::Sampled(9, id),
                            gpu::Bind::Sampled(10, id), gpu::Bind::Sampled(11, frame == 2 ? negative : prev),
                            gpu::Bind::Sampled(12, curr)});
      cmd.Push(push);
      cmd.Dispatch(1, 1, 1);
    });
    f32 values[12]{};
    check(device->ReadbackImage(moments, gpu::ResourceState::kGeneral, values, sizeof(values)),
          "read back temporal moments");
    for (f32 value : values) check(isfinite(value), "HDR moments must remain finite");
    check(::fabsf(values[5] - 1000000.0f) < 1.0f, "squared HDR luminance must not overflow");
    check(values[7] == 1.0f, "reset or invalid history must seed one frame");
    u16 half[12]{};
    check(device->ReadbackImage(accum, gpu::ResourceState::kGeneral, half, sizeof(half)),
          "read back accumulated radiance");
    for (u16 value : half) check((value & 0x7c00u) != 0x7c00u, "half output must remain finite");
  }
  device->WaitIdle();
  for (gpu::GpuImage& image : owned) device->DestroyImage(image);
  device->DestroyImage(accum);
  device->DestroyImage(moments);
  device->DestroyPipeline(pipeline);
  ::printf("recon_temporal_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
