#ifndef RX_RENDER_UTIL_IMGUI_RENDERER_H_
#define RX_RENDER_UTIL_IMGUI_RENDERER_H_

// A generic Dear ImGui *render* backend built entirely on the RHI, replacing
// raw imgui_impl_vulkan for RHI apps: font-atlas + user textures via
// CreateImage2D, a per-frame vertex/index ring, one blended pipeline from the
// embedded imgui shaders (optional frosted-glass backdrop, SetBackdrop),
// scissored indexed draws per ImDrawCmd. Honors dynamic textures
// (ImGuiBackendFlags_RendererHasTextures) and large-mesh vertex offsets.
//
// The platform side (imgui_impl_sdl3 or other) is separate and unchanged. Calls
// no global ImGui:: function (operates only on the ImDrawData given to
// Render()), so under RX_SHARED the app keeps one imgui context even with this
// backend in the render DSO. The app sets once, before the first Render:
//   io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures |
//                      ImGuiBackendFlags_RendererHasVtxOffset;
//
// Compiled only when the vendored imgui target exists; engine/render never
// hard-depends on imgui.

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

  // Frosted-glass backdrop: the pre-blurred copy of the scene behind the UI
  // (FrameView::blur_source / blur_sampler, produced by the renderer's ui_blur
  // pass when FrameView::needs_blur is set). Translucent widgets composite over
  // it instead of over the sharp frame. Call inside the ui_draw closure, before
  // Render; an invalid view turns the effect off for that frame.
  void SetBackdrop(TextureView blur, SamplerHandle sampler);

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
  // Frosted backdrop for the next Render, which consumes it: a frame that sets
  // none falls back to plain alpha blending rather than reusing a stale one.
  TextureView backdrop_;
  SamplerHandle backdrop_sampler_;
  FrameBuffers frames_[Device::kMaxFramesInFlight];
  u32 frame_index_ = 0;
  // Textures this backend created (owns the GPU backing behind BackendUserData),
  // tracked so Shutdown frees them without a global ImGui:: call (RX_SHARED keeps
  // one imgui context in the app DSO, not this one).
  std::vector<ImTextureData*> textures_;
};

}  // namespace rx::render

#endif  // RX_RENDER_UTIL_IMGUI_RENDERER_H_
