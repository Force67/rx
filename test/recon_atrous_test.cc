#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/rhi/device.h"
#include "shaders/recon_atrous_cs_hlsl.h"

using namespace rx;
using namespace rx::render;

namespace {
constexpr u32 kSize = 64, kPixels = kSize * kSize;
struct Push {
  u32 size[2] = {kSize, kSize};
  u32 step = 1;
  f32 normal_phi = 64, depth_phi = 80, luma_phi = 4;
  u32 spec_mode = 1;
  f32 spec_radius = 8;
};
f32 Half(u16 b) {
  const int exponent = (b >> 10) & 31;
  return std::ldexp(float((b & 1023) + (exponent ? 1024 : 0)),
                    exponent ? exponent - 25 : -24) * ((b & 0x8000) ? -1.f : 1.f);
}
}

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0 ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    std::printf("recon_atrous_test: SKIP, GPU unavailable\n");
    return 77;
  }
  PipelineHandle pipeline = device->CreateComputePipeline({
      .shader = RX_SHADER(k_recon_atrous_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}, {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage}, {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(Push)});
  if (!pipeline) return 1;
  std::vector<GpuImage> owned;
  auto input = [&](Format format, const std::vector<f32>& data) {
    GpuImage image = device->CreateImage2D(format, {kSize, kSize}, kTextureUsageSampled | kTextureUsageTransferDst);
    GpuBuffer staging = device->CreateBufferWithData(
        {reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(f32)}, kBufferUsageTransferSrc);
    if (!image || !staging) std::exit(1);
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
      BufferTextureCopy copy{.extent = {kSize, kSize}};
      cmd.CopyBufferToTexture(staging, image, {&copy, 1});
      cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadCompute));
    });
    device->DestroyBuffer(staging);
    owned.push_back(image);
    return image;
  };
  int failures = 0;
  // Flat noisy reflections, a noiseless sharp reflection, and a depth boundary.
  for (u32 mode = 0; mode < 3; ++mode) {
    std::vector<f32> colors(kPixels * 4), normals(kPixels * 4), depths(kPixels, 1);
    u32 rng = 7;
    for (u32 p = 0; p < kPixels; ++p) {
      rng = rng * 1664525u + 1013904223u;
      const f32 signal = mode && p % kSize >= kSize / 2 ? 3 : 1;
      const f32 noise = mode == 0 ? (f32(rng >> 8) / 16777216.f - .5f) : 0;
      for (u32 c = 0; c < 3; ++c) colors[p * 4 + c] = signal + noise;
      colors[p * 4 + 3] = mode == 1 ? 0 : 1.f / 12;
      normals[p * 4] = normals[p * 4 + 1] = .5f;
      normals[p * 4 + 2] = 1;
      normals[p * 4 + 3] = .2f;
      if (mode == 2 && p % kSize >= kSize / 2) depths[p] = 10;
    }
    GpuImage current = input(Format::kRGBA32Float, colors);
    GpuImage nr = input(Format::kRGBA32Float, normals);
    GpuImage vz = input(Format::kR32Float, depths);
    GpuImage last;
    for (u32 pass = 0; pass < 4; ++pass) {
      GpuImage output = device->CreateImage2D(Format::kRGBA16Float, {kSize, kSize},
          kTextureUsageStorage | kTextureUsageSampled | kTextureUsageTransferSrc);
      if (!output) return 1;
      owned.push_back(output);
      Push push;
      push.step = 1u << pass;
      device->ImmediateSubmit([&](CommandList& cmd) {
        cmd.Barrier(Transition(output, ResourceState::kUndefined, ResourceState::kGeneral));
        cmd.BindPipeline(pipeline);
        cmd.BindTransient(0, {Bind::Storage(0, output), Bind::Sampled(1, current),
                              Bind::Sampled(2, nr), Bind::Sampled(3, vz), Bind::Sampled(4, current)});
        cmd.Push(push);
        cmd.Dispatch(kSize / 8, kSize / 8, 1);
        cmd.Barrier(Transition(output, ResourceState::kGeneral, ResourceState::kShaderReadCompute));
      });
      current = last = output;
    }
    std::vector<u16> pixels(kPixels * 4);
    if (!device->ReadbackImage(last, ResourceState::kShaderReadCompute, pixels.data(), pixels.size() * sizeof(u16))) return 1;
    double mse = 0, mean = 0;
    bool finite = true;
    for (u32 p = 0; p < kPixels; ++p) {
      finite &= (pixels[p * 4] & 0x7c00u) != 0x7c00u;
      const double value = Half(pixels[p * 4]);
      const double expected = mode && p % kSize >= kSize / 2 ? 3 : 1;
      mse += (value - expected) * (value - expected) / kPixels;
      mean += value / kPixels;
    }
    std::printf("specular mode=%u RMS=%g mean=%g\n", mode, std::sqrt(mse), mean);
    const bool ok = finite && (mode == 0 ? std::sqrt(mse) < .14 && std::abs(mean - 1) < .03 : std::sqrt(mse) < .005);
    if (!ok) { std::printf("FAIL: reflection noise reduction / edge preservation\n"); ++failures; }
  }
  device->WaitIdle();
  for (auto& image : owned) device->DestroyImage(image);
  device->DestroyPipeline(pipeline);
  return failures ? 1 : 0;
}
