#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/post/depth_of_field.h"
#include "render/post/motion_blur.h"
#include "shaders/motion_tilemax_cs_hlsl.h"

using namespace rx;
using namespace rx::render;

namespace {
constexpr u32 kW = 256, kH = 128;
f32 Half(u16 b) {
  if ((b & 0x7c00u) == 0x7c00u) return INFINITY;
  const int exponent = (b >> 10) & 31;
  return std::ldexp(float((b & 1023) + (exponent ? 1024 : 0)),
                    exponent ? exponent - 25 : -24) * ((b & 0x8000) ? -1.f : 1.f);
}
struct TilePush {
  u32 tiles[2] = {kW / 16, kH / 16};
  u32 size[2] = {kW, kH};
  f32 scale[2] = {1, 1};
  f32 max_blur[2] = {1, 1};
  f32 debug[2] = {};
  f32 pad[2] = {};
};
}

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0 ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    std::printf("post_sampling_test: SKIP, GPU unavailable\n");
    return 77;
  }
  TransientPool pool(*device);
  std::vector<GpuImage> owned;
  auto input = [&](Format format, Extent2D extent, const std::vector<f32>& data) {
    GpuImage image = device->CreateImage2D(format, extent, kTextureUsageSampled | kTextureUsageTransferDst);
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
  std::vector<f32> colors(kW*kH*4);
  for (u32 y = 0; y < kH; ++y) for (u32 x = 0; x < kW; ++x) {
    for (u32 c = 0; c < 3; ++c) colors[(y*kW+x)*4+c] = (x/4) % 2 ? 1 : 0;
    colors[(y*kW+x)*4+3] = 1;
  }
  GpuImage color = input(Format::kRGBA32Float, {kW,kH}, colors);
  GpuImage depths[3];
  for (u32 i = 0; i < 3; ++i) {
    const u32 divisor = i == 1 ? 2 : 1;
    depths[i] = input(Format::kR32Float, {kW/divisor,kH/divisor},
                      std::vector<f32>(kW*kH/(divisor*divisor), i == 2 ? .025f : .05f));
  }
  int failures = 0;
  auto check = [&](bool ok, const char* name, double error) {
    std::printf("%s: %s, error=%g\n", name, ok ? "PASS" : "FAIL", error);
    if (!ok) ++failures;
  };
  auto render = [&](auto& pass, GpuImage guide, const auto& frame) {
    pool.BeginFrame();
    RenderGraph graph;
    ResourceState cs = ResourceState::kShaderReadCompute, gs = cs;
    auto c = graph.ImportImage("color", color, &cs);
    auto g = graph.ImportImage("guide", guide, &gs);
    auto out = pass.AddToGraph(graph, c, g, {kW,kH}, frame);
    graph.AddPass("readback", [out](RenderGraph::PassBuilder& b) {
      b.Read(out, ResourceUsage::kResolveSrc);
    }, [](PassContext&) {});
    if (!graph.Compile(*device, pool)) std::exit(1);
    device->ImmediateSubmit([&](CommandList& cmd) {
      PassContext ctx{.cmd = &cmd, .device = device.get(), .graph = &graph};
      graph.Execute(ctx);
    });
    std::vector<u16> bits(kW*kH*4);
    if (!device->ReadbackImage(graph.image(out), ResourceState::kResolveSrc,
                               bits.data(), bits.size()*sizeof(u16))) std::exit(1);
    std::vector<f32> pixels(bits.size());
    for (u32 i = 0; i < bits.size(); ++i) pixels[i] = Half(bits[i]);
    return pixels;
  };
  auto difference = [](const std::vector<f32>& a, const std::vector<f32>& b,
                       u32 left = 0, u32 right = kW, u32 top = 0, u32 bottom = kH) {
    double error = 0;
    for (u32 y = top; y < bottom; ++y) for (u32 x = left; x < right; ++x)
      for (u32 c = 0; c < 3; ++c) {
        const u32 i = (y*kW+x)*4+c;
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return double(INFINITY);
        error = std::max(error, double(std::abs(a[i]-b[i])));
      }
    return error;
  };
  for (u32 divisor = 1; divisor <= 2; ++divisor) {
    DepthOfFieldPass dof;
    if (!dof.Initialize(*device)) return 1;
    DepthOfFieldPass::Frame frame;
    frame.focus_distance = 2;
    frame.focus_speed = 1;
    frame.aperture = 16;
    auto pixels = render(dof, depths[divisor-1], frame);
    const double error = difference(pixels, colors);
    check(error < .001, divisor == 1 ? "DOF native focus" : "DOF upscaled focus", error);
    dof.Destroy(*device);
    DepthOfFieldPass automatic;
    if (!automatic.Initialize(*device)) return 1;
    frame.focus_distance = 0;
    pixels = render(automatic, depths[divisor-1], frame);
    const double auto_error = difference(pixels, colors);
    check(auto_error < .001, divisor == 1 ? "DOF native autofocus" : "DOF upscaled autofocus", auto_error);
    automatic.Destroy(*device);
  }
  {
    DepthOfFieldPass automatic, fixed;
    if (!automatic.Initialize(*device) || !fixed.Initialize(*device)) return 1;
    DepthOfFieldPass::Frame frame;
    frame.aperture = 16;
    frame.focus_speed = .5f;
    render(automatic, depths[0], frame);
    auto pixels = render(automatic, depths[2], frame);
    frame.focus_distance = 3;
    auto expected = render(fixed, depths[2], frame);
    double error = difference(pixels, expected);
    check(error < .001, "DOF uniform autofocus update", error);
    frame.focus_distance = 4;
    pixels = render(fixed, depths[2], frame);
    error = difference(pixels, colors);
    check(error < .001, "DOF explicit focus change", error);
    automatic.Destroy(*device);
    fixed.Destroy(*device);
  }
  MotionBlurPass blur;
  if (!blur.Initialize(*device)) return 1;
  MotionBlurPass::Frame frame;
  frame.shutter = 1;
  frame.samples = 32;
  std::vector<f32> native;
  for (u32 divisor = 1; divisor <= 2; ++divisor) {
    const u32 w = kW/divisor, h = kH/divisor;
    std::vector<f32> velocities(w*h*2);
    for (u32 y = 32/divisor; y < 96/divisor; ++y)
      for (u32 x = 192/divisor; x < w; ++x) velocities[(y*w+x)*2] = -16.f/kW;
    GpuImage motion = input(Format::kRG32Float, {w,h}, velocities);
    auto pixels = render(blur, motion, frame);
    if (divisor == 1) {
      native = pixels;
      const double changed = difference(pixels, colors, 208, 240, 48, 80);
      check(changed > .1, "Motion blur active", changed);
    } else {
      const double error = difference(pixels, native, 208, 240, 48, 80);
      check(error < .001, "Motion blur upscaled location", error);
    }
  }
  {
    std::vector<f32> velocities(kW*kH*2);
    auto pixels = render(blur, input(Format::kRG32Float, {kW,kH}, velocities), frame);
    double error = difference(pixels, colors);
    check(error < .001, "Motion blur static frame", error);
    for (u32 p = 0; p < kW*kH; ++p) velocities[p*2] = -12.f/kW;
    auto expected = render(blur, input(Format::kRG32Float, {kW,kH}, velocities), frame);
    for (u32 p = 0; p < kW*kH/2; ++p) {
      velocities[p*2] = 0;
      velocities[p*2+1] = -8.f/kH;
    }
    pixels = render(blur, input(Format::kRG32Float, {kW,kH}, velocities), frame);
    error = difference(pixels, expected, 64, 96, 64, 72);
    check(error < .001, "Motion blur aspect-correct neighborhood", error);
  }
  blur.Destroy(*device);
  // The longer screen-space vector must win, even on a non-square image.
  std::vector<f32> velocities(kW*kH*2);
  for (u32 p = 0; p < kW*kH; ++p) velocities[p*2+(p%2)] = p%2 ? -8.f/kH : -12.f/kW;
  GpuImage motion = input(Format::kRG32Float, {kW,kH}, velocities);
  GpuImage tiles = device->CreateImage2D(Format::kRG16Float, {kW/16,kH/16},
      kTextureUsageStorage | kTextureUsageTransferSrc);
  PipelineHandle pipeline = device->CreateComputePipeline({
      .shader = RX_SHADER(k_motion_tilemax_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}, {1, BindingType::kSampledImage}}}},
      .push_constant_size = PushSize<TilePush>()});
  if (!tiles || !pipeline) return 1;
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(tiles, ResourceState::kUndefined, ResourceState::kGeneral));
    cmd.BindPipeline(pipeline);
    cmd.BindTransient(0, {Bind::Storage(0, tiles), Bind::Sampled(1, motion)});
    cmd.Push(TilePush{});
    cmd.Dispatch2D({kW/16,kH/16});
  });
  std::vector<u16> bits(kW/16*kH/16*2);
  if (!device->ReadbackImage(tiles, ResourceState::kGeneral, bits.data(), bits.size()*sizeof(u16))) return 1;
  double error = 0;
  for (u32 p = 0; p < bits.size()/2; ++p)
    error = std::max(error, double(std::abs(Half(bits[p*2])-12.f/kW) + std::abs(Half(bits[p*2+1]))));
  check(error < .0001, "Motion blur aspect-correct maximum", error);
  device->WaitIdle();
  device->DestroyPipeline(pipeline);
  device->DestroyImage(tiles);
  for (auto& image : owned) device->DestroyImage(image);
  return failures ? 1 : 0;
}
