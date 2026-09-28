#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/algorithm.h"
#include "base/containers/vector.h"
#include "base/memory/mem_ops.h"
#include "foundation/math/scalar.h"
#include "rxe/render/post/post.h"

using namespace rx;
using namespace rx::render;
namespace gpu = rx::gpu;

int main() {
  constexpr u32 w = 128, h = 96;
  gpu::DeviceDesc desc;
  const char* backend = ::getenv("RX_RHI");
  desc.backend = backend && ::strcmp(backend, "d3d12") == 0 ? gpu::Backend::kD3D12 : gpu::Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = gpu::Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    ::printf("lens_flare_test: SKIP, GPU unavailable\n");
    return 77;
  }
  auto post = PostPass::Create(*device, gpu::Format::kRGBA32Float);
  if (!post) return 1;
  gpu::GpuImage scene = device->CreateImage2D(gpu::Format::kRGBA32Float, {w,h}, gpu::kTextureUsageSampled | gpu::kTextureUsageTransferDst);
  gpu::GpuImage flare = device->CreateImage2D(gpu::Format::kRGBA32Float, {w,h}, gpu::kTextureUsageSampled | gpu::kTextureUsageTransferDst);
  gpu::GpuImage output = device->CreateImage2D(gpu::Format::kRGBA32Float, {w,h}, gpu::kTextureUsageColorTarget | gpu::kTextureUsageTransferSrc);
  gpu::GpuBuffer exposure = device->CreateBuffer(sizeof(f32), gpu::kBufferUsageStorage, true);
  if (!scene || !flare || !output || !exposure.mapped) return 1;
  auto upload = [&](gpu::GpuImage image, const base::Vector<f32>& data, gpu::ResourceState state) {
    gpu::GpuBuffer staging = device->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(f32)), gpu::kBufferUsageTransferSrc);
    device->ImmediateSubmit([&](gpu::CommandList& cmd) {
      cmd.Barrier(gpu::Transition(image, state, gpu::ResourceState::kCopyDst));
      gpu::BufferTextureCopy copy{.extent = {w,h}};
      cmd.CopyBufferToTexture(staging, image, base::Span(&copy, 1));
      cmd.Barrier(gpu::Transition(image, gpu::ResourceState::kCopyDst, gpu::ResourceState::kShaderReadFragment));
    });
    device->DestroyBuffer(staging);
  };
  base::Vector<f32> data(w*h*4), pixels(w*h*4), reference;
  upload(scene, data, gpu::ResourceState::kUndefined);
  int failures = 0;
  gpu::ResourceState flare_state = gpu::ResourceState::kUndefined, output_state = gpu::ResourceState::kUndefined;
  for (u32 mode = 0; mode < 6; ++mode) {
    base::Fill(data.begin(), data.end(), 0.f);
    const f32 gain = mode == 2 ? 8 : 1;
    // A bright object away from the optical axis. No bloom or scene energy is
    // present, so every output pixel measures the production flare itself.
    const u32 left = mode == 5 ? 0 : 92;
    for (u32 y = 42; y < 54; ++y) for (u32 x = left; x < left + 12; ++x)
      for (u32 c = 0; c < 3; ++c) data[(y*w+x)*4+c] = (mode == 0 ? 2.5f : 16.f) / gain;
    if (mode == 3) base::Fill(data.begin(), data.end(), 0.f);
    upload(flare, data, flare_state);
    flare_state = gpu::ResourceState::kShaderReadFragment;
    base::MemCopy(exposure.mapped, &gain, sizeof(gain));
    device->FlushBuffer(exposure, 0, sizeof(gain));
    device->ImmediateSubmit([&](gpu::CommandList& cmd) {
      cmd.Barrier(gpu::Transition(output, output_state, gpu::ResourceState::kColorTarget));
      PassContext ctx{.cmd = &cmd, .device = device.Get_UseOnlyIfYouKnowWhatYouareDoing()};
      PostPass::Params params;
      params.tonemap = 2;
      params.output_transfer = 2;
      params.paper_white = 80;
      params.flare_intensity = mode == 4 ? 0 : .1f;
      post->Record(ctx, scene.view, scene.view, flare.view, exposure, sizeof(f32), output.view, {w,h}, params);
    });
    if (!device->ReadbackImage(output, gpu::ResourceState::kColorTarget, pixels.data(), pixels.size()*sizeof(f32))) return 1;
    output_state = gpu::ResourceState::kCopySrc;
    double sum = 0, outside = 0, difference = 0;
    for (u32 y = 0; y < h; ++y) for (u32 x = 0; x < w; ++x) {
      bool expected = false;
      for (f32 scale : {-.35f, -.65f, .4f, .8f}) {
        const f32 source_x = .5f + ((x+.5f)/w-.5f)/scale;
        const f32 source_y = .5f + ((y+.5f)/h-.5f)/scale;
        expected |= source_x >= (float(left)-1)/w && source_x <= (float(left)+13)/w && source_y >= 41.f/h && source_y <= 55.f/h;
      }
      for (u32 c = 0; c < 3; ++c) {
        const u32 index = (y*w+x)*4+c;
        const f32 value = pixels[index];
        if (!isfinite(value)) ++failures;
        sum += ::fabsf(value);
        if (!expected) outside += ::fabsf(value);
        if (mode == 2) difference = rx::Max(difference, double(::fabsf(value-reference[index])));
      }
    }
    ::printf("flare mode=%u energy=%g outside_ghosts=%g exposure_error=%g\n", mode, sum, outside, difference);
    bool ok = true;
    if (mode == 0 || mode == 3 || mode == 4) ok = sum < 1e-6;
    if (mode == 1) { ok = sum > .1 && outside < 1e-5; reference = pixels; }
    if (mode == 2) ok = difference < 1e-5;
    if (mode == 5) ok = sum > .1 && outside < 1e-5;
    if (!ok) { ++failures; ::printf("FAIL: flare highlight selection, footprint, or exposure invariance\n"); }
  }
  device->WaitIdle();
  post.Reset();
  device->DestroyBuffer(exposure);
  device->DestroyImage(scene);
  device->DestroyImage(flare);
  device->DestroyImage(output);
  return failures ? 1 : 0;
}
