// Offscreen RHI acceptance test: create a surfaceless device through the RHI,
// clear+draw a triangle into an RGBA8 GpuImage, read it back and assert the
// pixels (red triangle at the centre, blue background in the corners).
//
// The backend follows RX_RHI (vulkan|d3d12; default vulkan), so the same binary
// validates both paths — d3d12 runs against vkd3d on linux. Skips cleanly
// (exit 0) when no driver is present: CreateOffscreen then returns a
// null-backend stub. Run under vkrun to exercise the real GPU path.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

// Build-embedded test shaders (test/shaders/*.hlsl -> generated/shaders/*.h).
#include "shaders/offscreen_tri_vs_hlsl.h"
#include "shaders/offscreen_tri_ps_hlsl.h"

using namespace rx::render;

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "offscreen_test: FAIL: %s\n", msg);
  return 1;
}

}  // namespace

int main() {
  DeviceDesc desc;
  const char* rhi = std::getenv("RX_RHI");
  desc.backend = (rhi && std::strcmp(rhi, "d3d12") == 0) ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = false;  // not needed; keeps the adapter requirements minimal
  std::unique_ptr<Device> device = Device::CreateOffscreen(desc);
  if (!device) return Fail("CreateOffscreen returned null");

  if (device->is_stub()) {
    // No driver for the requested backend on this machine (plain ctest without
    // vkrun). A skip, not a failure - the real pixel path is proven under vkrun.
    std::printf("offscreen_test: no %s driver, skipping (null backend)\n",
                BackendName(desc.backend));
    return 0;
  }

  std::printf("offscreen_test: device '%s'\n", device->caps().adapter_name.c_str());

  constexpr u32 kW = 64, kH = 64;
  const Format kFmt = Format::kRGBA8Unorm;

  GpuImage image = device->CreateImage2D(
      kFmt, {kW, kH},
      kTextureUsageColorTarget | kTextureUsageTransferSrc);
  if (!image) return Fail("CreateImage2D returned null");

  GraphicsPipelineDesc pd;
  pd.vertex = RX_SHADER(k_offscreen_tri_vs_hlsl);
  pd.fragment = RX_SHADER(k_offscreen_tri_ps_hlsl);
  pd.color_formats.push_back(kFmt);
  pd.raster.cull = CullMode::kNone;  // winding-agnostic
  pd.debug_name = "offscreen_tri";
  PipelineHandle pipeline = device->CreateGraphicsPipeline(pd);
  if (!pipeline) return Fail("CreateGraphicsPipeline returned null");

  const f32 kBg[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // blue background

  CommandList* cmd = device->BeginFrame(0);
  if (!cmd) return Fail("BeginFrame returned null");
  cmd->Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kColorTarget));
  ColorAttachment color{.view = image.view, .load = LoadOp::kClear, .store = StoreOp::kStore};
  color.clear[0] = kBg[0];
  color.clear[1] = kBg[1];
  color.clear[2] = kBg[2];
  color.clear[3] = kBg[3];
  cmd->BeginRendering({.extent = {kW, kH}, .colors = {&color, 1}});
  cmd->BindPipeline(pipeline);
  cmd->Draw(3);
  cmd->EndRendering();
  device->SubmitFrame(cmd);  // swapchainless
  device->WaitIdle();

  std::vector<u8> pixels(static_cast<size_t>(kW) * kH * 4);
  if (!device->ReadbackImage(image, ResourceState::kColorTarget, pixels.data(), pixels.size()))
    return Fail("ReadbackImage returned false");

  auto at = [&](u32 x, u32 y) -> const u8* {
    return &pixels[(static_cast<size_t>(y) * kW + x) * 4];
  };

  // Centre: the red triangle. R high, B low.
  const u8* c = at(kW / 2, kH / 2);
  std::printf("offscreen_test: center rgba = %u %u %u %u\n", c[0], c[1], c[2], c[3]);
  if (!(c[0] > 200 && c[2] < 60)) return Fail("center pixel is not the red triangle");

  // A corner: the cleared blue background. B high, R low.
  const u8* corner = at(0, 0);
  std::printf("offscreen_test: corner rgba = %u %u %u %u\n", corner[0], corner[1], corner[2],
              corner[3]);
  if (!(corner[2] > 200 && corner[0] < 60)) return Fail("corner pixel is not the background");

  device->DestroyPipeline(pipeline);
  device->DestroyImage(image);

  std::printf("offscreen_test: PASS\n");
  return 0;
}
