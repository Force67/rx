#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/post/antialiasing.h"
#include "render/post/upscaler.h"
#include "render/rhi/device.h"
#if defined(RX_HAS_DLSS) && !defined(__aarch64__)
#include "render/gi/denoiser_rr.h"
#endif

using namespace rx;
using namespace rx::render;

namespace {
constexpr u32 kW = 96, kH = 64, kOutW = 192, kOutH = 128;
float Pattern(float x, float y, u32 channel) {
  if (channel == 0) return .5f + .4f * std::sin(x * 1.1f);
  if (channel == 1) return .5f + .4f * std::sin(y * .9f);
  return .5f + .3f * std::sin(x * .6f + y * .7f);
}
float Half(u16 bits) {
  return std::ldexp(float((bits & 1023) | 1024), int((bits >> 10) & 31) - 25) *
         ((bits & 0x8000) ? -1.f : 1.f);
}
}  // namespace

int main() {
  DeviceDesc desc;
  desc.request_raytracing = false;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) return 0;
  const char* name = std::getenv("RX_TEST_UPSCALER");
  const UpscalerKind kind = name && std::strcmp(name, "dlss") == 0 ? UpscalerKind::kDlss : UpscalerKind::kFsr3;
  auto upscaler = CreateUpscaler({.kind = kind, .render_width = kW, .render_height = kH,
                                 .output_width = kOutW, .output_height = kOutH}, *device);
  if (!upscaler) { std::printf("upscaler_motion_test: SKIP, backend unavailable\n"); return 0; }
#if defined(RX_HAS_DLSS) && !defined(__aarch64__)
  if (kind == UpscalerKind::kDlss) {
    RrDenoiser rr;
    if (!rr.Initialize(*device, {kW, kH})) return 1;
    rr.Resize(*device, {kOutW, kOutH});
    if (!rr.available()) return 1;
    rr.Destroy(*device);
  }
#endif
  TransientPool pool(*device);
  auto make_image = [&](Format format) {
    return device->CreateImage2D(format, {kW, kH}, kTextureUsageSampled | kTextureUsageTransferDst);
  };
  GpuImage color = make_image(Format::kRGBA32Float);
  GpuImage depth = make_image(Format::kR32Float);
  GpuImage motion = make_image(Format::kRG32Float);
  if (!color || !depth || !motion) return 1;
  ResourceState color_state = ResourceState::kUndefined, depth_state = ResourceState::kUndefined,
                motion_state = ResourceState::kUndefined;
  std::vector<f32> colors(kW * kH * 4), depths(kW * kH, .1f), motions(kW * kH * 2, 0);
  auto upload = [&](GpuImage image, ResourceState& state, const std::vector<f32>& data) {
    GpuBuffer staging = device->CreateBufferWithData(
        {reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(f32)}, kBufferUsageTransferSrc);
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(image, state, ResourceState::kCopyDst));
      BufferTextureCopy copy{.extent = {kW, kH}};
      cmd.CopyBufferToTexture(staging, image, {&copy, 1});
    });
    state = ResourceState::kCopyDst;
    device->DestroyBuffer(staging);
  };
  upload(depth, depth_state, depths);
  upload(motion, motion_state, motions);
  u32 frame_index = 0;
  auto measure = [&](f32 jitter_sign, f32 velocity, f32 motion_sign, u32 reset_mode = 0) {
    double error = 0;
    for (u32 frame = 0; frame < 32; ++frame, ++frame_index) {
      if (reset_mode == 1) frame_index += 2;
      f32 jx, jy;
      JitterSequence::Sample(frame, 32, &jx, &jy);
      for (u32 y = 0; y < kH; ++y) for (u32 x = 0; x < kW; ++x) {
        for (u32 c = 0; c < 3; ++c)
          colors[4 * (y * kW + x) + c] = Pattern(x + .5f - jx - velocity * frame, y + .5f - jy, c);
        colors[4 * (y * kW + x) + 3] = 1;
        motions[2 * (y * kW + x)] = motion_sign * velocity / kW;
      }
      upload(color, color_state, colors);
      upload(motion, motion_state, motions);
      RenderGraph graph;
      pool.BeginFrame();
      UpscalerInputs inputs;
      inputs.color = graph.ImportImage("color", color, &color_state);
      inputs.depth = graph.ImportImage("depth", depth, &depth_state);
      inputs.motion_vectors = graph.ImportImage("motion", motion, &motion_state);
      inputs.jitter_x = jitter_sign * jx;
      inputs.jitter_y = jitter_sign * jy;
      inputs.frame_delta_seconds = 1.f / 60;
      inputs.reset_history = frame == 0 || reset_mode == 2;
      inputs.frame_index = frame_index;
      ResourceHandle output = upscaler->AddToGraph(graph, inputs);
      graph.AddPass("readback_state", [output](RenderGraph::PassBuilder& b) {
        b.Read(output, ResourceUsage::kResolveSrc);
      }, [](PassContext&) {});
      if (!graph.Compile(*device, pool)) std::exit(1);
      device->ImmediateSubmit([&](CommandList& cmd) {
        PassContext ctx{.cmd = &cmd, .device = device.get(), .graph = &graph};
        graph.Execute(ctx);
      });
      if (frame >= 24) {
        std::vector<u16> pixels(kOutW * kOutH * 4);
        if (!device->ReadbackImage(graph.image(output), ResourceState::kResolveSrc,
                                   pixels.data(), pixels.size() * sizeof(u16))) std::exit(1);
        for (u32 y = 16; y < kOutH - 16; ++y) for (u32 x = 16; x < kOutW - 16; ++x)
          for (u32 c = 0; c < 3; ++c) {
            const u16 bits = pixels[4 * (y * kOutW + x) + c];
            if ((bits & 0x7c00u) == 0x7c00u) {
              std::printf("FAIL: upscaler produced non-finite color\n");
              std::exit(1);
            }
            double delta = Half(bits) -
                Pattern((x + .5f) * .5f - velocity * frame, (y + .5f) * .5f, c);
            error += delta * delta;
          }
      }
    }
    return error / (8 * (kOutW - 32) * (kOutH - 32) * 3);
  };
  const double positive = measure(1, 0, 0);
  const double negative = measure(-1, 0, 0);
  const double moving_correct = measure(1, .75f, -1);
  const double moving_wrong = measure(1, .75f, 1);
  std::printf("upscaler_motion_test %s: jitter positive MSE=%g negative MSE=%g; motion correct=%g reversed=%g\n",
               UpscalerName(kind), positive, negative, moving_correct, moving_wrong);
  const double resumed = measure(1, 0, 0, 1);
  const double reset = measure(1, 0, 0, 2);
  std::printf("skipped-frame reset MSE=%g explicit-reset MSE=%g\n", resumed, reset);
  device->WaitIdle();
  upscaler.reset();
  pool.Clear();
  device->DestroyImage(color);
  device->DestroyImage(depth);
  device->DestroyImage(motion);
  return positive < negative && moving_correct < moving_wrong && std::abs(resumed - reset) < 1e-6 ? 0 : 1;
}
