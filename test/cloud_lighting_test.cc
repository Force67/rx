#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/atmosphere/clouds.h"
#include "render/rhi/device.h"

using namespace rx;
using namespace rx::render;

namespace {
f32 Half(u16 bits) {
  if ((bits & 0x7c00u) == 0x7c00u) return INFINITY;
  const int exponent = (bits >> 10) & 31;
  return std::ldexp(float((bits & 1023) + (exponent ? 1024 : 0)),
                    exponent ? exponent - 25 : -24) * ((bits & 0x8000) ? -1.f : 1.f);
}
}  // namespace

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0 ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    std::printf("cloud_lighting_test: SKIP, GPU unavailable\n");
    return 77;
  }
  Clouds clouds;
  if (!clouds.Initialize(*device)) return 1;
  TransientPool pool(*device);
  int failures = 0;
  auto check = [&](bool ok, const char* name, double error) {
    std::printf("%s: %s, error=%g\n", name, ok ? "PASS" : "FAIL", error);
    if (!ok) ++failures;
  };
  for (Extent2D extent : {Extent2D{32, 16}, Extent2D{37, 23}}) {
    std::vector<GpuImage> owned;
    const u32 count = extent.width * extent.height;
    auto input = [&](Format format, const std::vector<f32>& data) {
      GpuImage image = device->CreateImage2D(format, extent,
          kTextureUsageSampled | kTextureUsageTransferDst);
      GpuBuffer staging = device->CreateBufferWithData(
          {reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(f32)}, kBufferUsageTransferSrc);
      if (!image || !staging) std::exit(1);
      device->ImmediateSubmit([&](CommandList& cmd) {
        cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
        BufferTextureCopy copy{.extent = extent};
        cmd.CopyBufferToTexture(staging, image, {&copy, 1});
        cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadCompute));
      });
      device->DestroyBuffer(staging);
      owned.push_back(image);
      return image;
    };
    const GpuImage black = input(Format::kRGBA32Float, std::vector<f32>(count * 4, 0));
    const GpuImage white = input(Format::kRGBA32Float, std::vector<f32>(count * 4, 1));
    const GpuImage depth = input(Format::kR32Float, std::vector<f32>(count, 1.f / 5000));
    const GpuImage foreground = input(Format::kR32Float, std::vector<f32>(count, 1));
    Clouds::Frame frame;
    // Parallel upward rays: world y = 1/depth. The surface at 5000 m clips
    // cirrus, leaving only the cumulus layer and its known input background.
    frame.inv_view_proj = {};
    frame.inv_view_proj.m[13] = 1;
    frame.inv_view_proj.m[11] = 1;
    frame.camera_pos = {0, 0, 0};
    frame.sun_direction = {0, -1, 0};
    frame.sun_color = {0, 0, 0};
    frame.coverage = 1;
    frame.steps = 1;
    frame.light_steps = 0;
    auto render = [&](GpuImage color, GpuImage guide) {
      pool.BeginFrame();
      RenderGraph graph;
      ResourceState cs = ResourceState::kShaderReadCompute, ds = cs;
      auto c = graph.ImportImage("color", color, &cs);
      auto d = graph.ImportImage("depth", guide, &ds);
      auto out = clouds.AddToGraph(graph, c, d, extent, frame);
      graph.AddPass("readback", [out](RenderGraph::PassBuilder& b) {
        b.Read(out, ResourceUsage::kResolveSrc);
      }, [](PassContext&) {});
      if (!graph.Compile(*device, pool)) std::exit(1);
      device->ImmediateSubmit([&](CommandList& cmd) {
        PassContext ctx{.cmd = &cmd, .device = device.get(), .graph = &graph};
        graph.Execute(ctx);
      });
      std::vector<u16> bits(count * 4);
      if (!device->ReadbackImage(graph.image(out), ResourceState::kResolveSrc,
                                 bits.data(), bits.size() * sizeof(u16))) std::exit(1);
      std::vector<f32> pixels(bits.size());
      for (u32 i = 0; i < bits.size(); ++i) pixels[i] = Half(bits[i]);
      return pixels;
    };

    // One sample fixes position and illumination. Changing density changes
    // opacity, but must not change the recovered scattering source. Rendering
    // over black and white independently measures transmission as white-black.
    frame.density = .05f;
    auto dark_a = render(black, depth);
    auto light_a = render(white, depth);
    frame.density = .1f;
    auto dark_b = render(black, depth);
    auto light_b = render(white, depth);
    double error = 0, min_source = INFINITY;
    u32 compared = 0;
    for (u32 p = 0; p < count; ++p) {
      for (u32 c = 0; c < 3; ++c) {
        const u32 i = p * 4 + c;
        const f32 opacity_a = 1 - (light_a[i] - dark_a[i]);
        const f32 opacity_b = 1 - (light_b[i] - dark_b[i]);
        if (!std::isfinite(opacity_a) || !std::isfinite(opacity_b)) {
          error = INFINITY;
          continue;
        }
        if (opacity_a < .1f || opacity_b < .1f) continue;
        const f32 source_a = dark_a[i] / opacity_a;
        const f32 source_b = dark_b[i] / opacity_b;
        min_source = std::min(min_source, double(std::min(source_a, source_b)));
        error = std::max(error, double(std::abs(source_a - source_b) /
                                       std::max(source_b, 1e-6f)));
        ++compared;
      }
    }
    check(compared > count && error < .025, "Cloud source independent of density", error);
    check(std::isfinite(min_source) && min_source > .1, "Cloud ambient illumination preserved", min_source);
    std::printf("extent=%ux%u, compared=%u channels\n", extent.width, extent.height, compared);

    auto expect = [&](const std::vector<f32>& pixels, f32 value, const char* name) {
      double difference = 0;
      for (u32 p = 0; p < count; ++p) for (u32 c = 0; c < 3; ++c) {
        f32 sample = pixels[p * 4 + c];
        difference = std::isfinite(sample) ? std::max(difference, double(std::abs(sample - value))) : INFINITY;
      }
      check(difference < .001, name, difference);
    };
    expect(render(white, foreground), 1, "Foreground occludes clouds");
    frame.sun_intensity = 0;
    expect(render(black, depth), 0, "Unlit clouds emit no light");
    frame.sun_intensity = 4;
    frame.density = 0;
    expect(render(white, depth), 1, "Empty clouds preserve background");
    for (auto image : owned) device->DestroyImage(image);
  }
  device->WaitIdle();
  clouds.Destroy(*device);
  return failures ? 1 : 0;
}
