#ifndef RX_RENDER_UTIL_IMGUI_RENDERER_H_
#define RX_RENDER_UTIL_IMGUI_RENDERER_H_

// A generic, reusable Dear ImGui *render* backend built entirely on the RHI
// (engine/render/rhi). It replaces the raw imgui_impl_vulkan backend for any
// RHI-based app: font-atlas + user textures via CreateImage2D, a per-frame
// vertex/index ring, one alpha-blended pipeline from the engine's embedded imgui
// shaders, and scissored indexed draws per ImDrawCmd. Dynamic textures
// (ImGuiBackendFlags_RendererHasTextures) and large-mesh vertex offsets are
// honored.
//
// The platform side (imgui_impl_sdl3 or another) is separate and unchanged; this
// is only the renderer. It deliberately calls no global ImGui:: function - it
// operates purely on the ImDrawData handed to Render() - so under RX_SHARED the
// app keeps a single imgui context even though this backend lives in the render
// DSO. The app therefore owns two small responsibilities on its context:
//   io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures |
//                      ImGuiBackendFlags_RendererHasVtxOffset;
// (set once, before the first Render).
//
// Compiled only when the vendored imgui target exists (see the render module's
// CMakeLists); engine/render never hard-depends on imgui.

#include <cstddef>
#include <vector>

#include "core/export.h"
#include "render/rhi/device.h"
#include "render/rhi/types.h"

struct ImDrawData;      // <imgui.h>, included only by the implementation
struct ImTextureData;

namespace rx::render {

class CommandList;

class RX_RENDER_EXPORT ImGuiRenderer {
 public:
  ImGuiRenderer() = default;
  ~ImGuiRenderer();

  ImGuiRenderer(const ImGuiRenderer&) = delete;
  ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

  // Builds the pipeline (targeting target_format, the color attachment format
  // the draws render into, e.g. the swapchain format), the shared sampler and
  // the per-frame vertex/index rings. Returns false on a stub device or when
  // the pipeline fails to build.
  bool Initialize(Device& device, Format target_format);

  // Records draw_data into cmd. Call inside an open dynamic-rendering pass whose
  // single color attachment matches target_format, and between the device's
  // BeginFrame and SubmitFrame (per-draw texture binds use the frame's transient
  // pool). Texture create/update/destroy requests carried in draw_data are
  // serviced first.
  void Render(ImDrawData* draw_data, CommandList& cmd);

  void Shutdown();

  bool initialized() const { return device_ != nullptr; }

 private:
  struct FrameBuffers {
    GpuBuffer vertices;
    GpuBuffer indices;
  };

  void UpdateTexture(ImTextureData* tex);
  void DestroyTexture(ImTextureData* tex);

  Device* device_ = nullptr;
  Format target_format_ = Format::kUnknown;
  PipelineHandle pipeline_;
  SamplerHandle sampler_;
  FrameBuffers frames_[Device::kMaxFramesInFlight];
  u32 frame_index_ = 0;
  // Textures this backend created (owns the GPU backing behind BackendUserData),
  // tracked so Shutdown frees them without a global ImGui:: call (RX_SHARED keeps
  // one imgui context in the app DSO, not this one).
  std::vector<ImTextureData*> textures_;
};

}  // namespace rx::render

#endif  // RX_RENDER_UTIL_IMGUI_RENDERER_H_
