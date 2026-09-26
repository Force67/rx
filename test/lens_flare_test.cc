#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/algorithm.h"
#include "base/containers/vector.h"
#include "base/memory/mem_ops.h"
#include "core/scalar.h"
#include "render/post/post.h"

using namespace rx;
using namespace rx::render;

int main() {
  constexpr u32 w = 128, h = 96;
  DeviceDesc desc;
  const char* backend = ::getenv("RX_RHI");
  desc.backend = backend && ::strcmp(backend, "d3d12") == 0 ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    ::printf("lens_flare_test: SKIP, GPU unavailable\n");
    return 77;
  }
  auto post = PostPass::Create(*device, Format::kRGBA32Float);
  if (!post) return 1;
  GpuImage scene = device->CreateImage2D(Format::kRGBA32Float, {w,h}, kTextureUsageSampled | kTextureUsageTransferDst);
  GpuImage flare = device->CreateImage2D(Format::kRGBA32Float, {w,h}, kTextureUsageSampled | kTextureUsageTransferDst);
  GpuImage output = device->CreateImage2D(Format::kRGBA32Float, {w,h}, kTextureUsageColorTarget | kTextureUsageTransferSrc);
  GpuBuffer exposure = device->CreateBuffer(sizeof(f32), kBufferUsageStorage, true);
  if (!scene || !flare || !output || !exposure.mapped) return 1;
  auto upload = [&](GpuImage image, const base::Vector<f32>& data, ResourceState state) {
    GpuBuffer staging = device->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(f32)), kBufferUsageTransferSrc);
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(image, state, ResourceState::kCopyDst));
      BufferTextureCopy copy{.extent = {w,h}};
      cmd.CopyBufferToTexture(staging, image, base::Span(&copy, 1));
      cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadFragment));
    });
    device->DestroyBuffer(staging);
  };
  base::Vector<f32> data(w*h*4), pixels(w*h*4), reference;
  upload(scene, data, ResourceState::kUndefined);
  int failures = 0;
  ResourceState flare_state = ResourceState::kUndefined, output_state = ResourceState::kUndefined;
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
    flare_state = ResourceState::kShaderReadFragment;
    base::MemCopy(exposure.mapped, &gain, sizeof(gain));
    device->FlushBuffer(exposure, 0, sizeof(gain));
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(output, output_state, ResourceState::kColorTarget));
      PassContext ctx{.cmd = &cmd, .device = device.Get_UseOnlyIfYouKnowWhatYouareDoing()};
      PostPass::Params params;
      params.tonemap = 2;
      params.output_transfer = 2;
      params.paper_white = 80;
      params.flare_intensity = mode == 4 ? 0 : .1f;
      post->Record(ctx, scene.view, scene.view, flare.view, exposure, sizeof(f32), output.view, {w,h}, params);
    });
    if (!device->ReadbackImage(output, ResourceState::kColorTarget, pixels.data(), pixels.size()*sizeof(f32))) return 1;
    output_state = ResourceState::kCopySrc;
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
        if (!::isfinite(value)) ++failures;
        sum += ::abs(value);
        if (!expected) outside += ::abs(value);
        if (mode == 2) difference = rx::Max(difference, double(::abs(value-reference[index])));
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
