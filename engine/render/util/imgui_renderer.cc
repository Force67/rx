#include "render/util/imgui_renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <imgui.h>

#include "core/log.h"
#include "render/rhi/bindings.h"
#include "render/rhi/command_list.h"
#include "render/rhi/pipeline.h"

// Build-embedded imgui shaders (engine/render/shaders/util/imgui.{vs,ps}.hlsl).
#include "shaders/imgui_vs_hlsl.h"
#include "shaders/imgui_ps_hlsl.h"

namespace rx::render {
namespace {

struct ImGuiPush {
  f32 scale[2];
  f32 translate[2];
};

// Backing GPU resource behind an ImTextureData (stashed in BackendUserData).
struct BackendTexture {
  GpuImage image;
};

}  // namespace

ImGuiRenderer::~ImGuiRenderer() { Shutdown(); }

bool ImGuiRenderer::Initialize(Device& device, Format target_format) {
  if (device.is_stub()) return false;
  device_ = &device;
  target_format_ = target_format;

  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .mip_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});

  GraphicsPipelineDesc pd;
  pd.vertex = RX_SHADER(k_imgui_vs_hlsl);
  pd.fragment = RX_SHADER(k_imgui_ps_hlsl);

  VertexBufferLayout vb;
  vb.stride = static_cast<u32>(sizeof(ImDrawVert));
  vb.attributes.push_back(
      {.location = 0, .format = Format::kRG32Float, .offset = offsetof(ImDrawVert, pos)});
  vb.attributes.push_back(
      {.location = 1, .format = Format::kRG32Float, .offset = offsetof(ImDrawVert, uv)});
  vb.attributes.push_back(
      {.location = 2, .format = Format::kRGBA8Unorm, .offset = offsetof(ImDrawVert, col)});
  pd.vertex_buffers.push_back(std::move(vb));

  pd.color_formats.push_back(target_format);
  pd.blend.push_back(BlendMode::kAlpha);
  pd.raster.cull = CullMode::kNone;
  pd.depth.format = Format::kUnknown;  // no depth

  PipelineBindings set0;
  set0.stages = kShaderStageFragment;
  set0.slots.push_back({.binding = 0, .type = BindingType::kCombinedTextureSampler});
  pd.sets.push_back(std::move(set0));

  pd.push_constant_size = sizeof(ImGuiPush);
  pd.debug_name = "imgui";
  pipeline_ = device.CreateGraphicsPipeline(pd);
  if (!pipeline_) {
    RX_ERROR("imgui renderer: pipeline creation failed");
    device_ = nullptr;
    return false;
  }
  return true;
}

void ImGuiRenderer::DestroyTexture(ImTextureData* tex) {
  auto* backend = static_cast<BackendTexture*>(tex->BackendUserData);
  if (!backend) return;
  device_->DestroyImageDeferred(backend->image);
  delete backend;
  tex->SetTexID(ImTextureID_Invalid);
  tex->BackendUserData = nullptr;
  tex->SetStatus(ImTextureStatus_Destroyed);
  textures_.erase(std::remove(textures_.begin(), textures_.end(), tex), textures_.end());
}

void ImGuiRenderer::UpdateTexture(ImTextureData* tex) {
  if (tex->Status == ImTextureStatus_WantDestroy) {
    if (tex->UnusedFrames > 0) DestroyTexture(tex);
    return;
  }
  if (tex->Status != ImTextureStatus_WantCreate && tex->Status != ImTextureStatus_WantUpdates)
    return;
  // Default atlas format is RGBA32 (4 bytes/texel); that is all this backend
  // uploads (the engine's fonts stay RGBA32).
  if (tex->Format != ImTextureFormat_RGBA32 || tex->BytesPerPixel != 4) {
    RX_WARN("imgui renderer: unsupported texture format, skipping");
    tex->SetStatus(ImTextureStatus_OK);
    return;
  }

  const bool create = tex->Status == ImTextureStatus_WantCreate;
  auto* backend = static_cast<BackendTexture*>(tex->BackendUserData);
  if (create || !backend) {
    if (!backend) backend = new BackendTexture();
    backend->image = device_->CreateImage2D(
        Format::kRGBA8Unorm, {static_cast<u32>(tex->Width), static_cast<u32>(tex->Height)},
        kTextureUsageSampled | kTextureUsageTransferDst);
    tex->BackendUserData = backend;
    tex->SetTexID(static_cast<ImTextureID>(backend->image.view.value));
    textures_.push_back(tex);
  }

  // Upload the full pixel buffer (tex->Pixels always holds the whole texture);
  // simple and correct - imgui only issues updates when glyphs are added.
  const u64 size = static_cast<u64>(tex->Width) * tex->Height * 4;
  GpuBuffer staging = device_->CreateBuffer(size, kBufferUsageTransferSrc, true);
  std::memcpy(staging.mapped, tex->GetPixels(), size);
  device_->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(backend->image,
                           create ? ResourceState::kUndefined : ResourceState::kShaderReadFragment,
                           ResourceState::kCopyDst));
    BufferTextureCopy region{};  // whole mip 0 at the origin
    cmd.CopyBufferToTexture(staging, backend->image, {&region, 1});
    cmd.Barrier(Transition(backend->image, ResourceState::kCopyDst,
                           ResourceState::kShaderReadFragment));
  });
  device_->DestroyBuffer(staging);
  tex->SetStatus(ImTextureStatus_OK);
}

void ImGuiRenderer::Render(ImDrawData* draw_data, CommandList& cmd) {
  if (!device_ || !pipeline_) return;

  // Service texture create/update/destroy requests before drawing.
  if (draw_data->Textures != nullptr) {
    for (ImTextureData* tex : *draw_data->Textures)
      if (tex->Status != ImTextureStatus_OK) UpdateTexture(tex);
  }

  const int fb_width = static_cast<int>(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
  const int fb_height = static_cast<int>(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
  if (fb_width <= 0 || fb_height <= 0 || draw_data->TotalVtxCount <= 0) return;

  // Per-frame vertex/index ring: grow (via the deferred-destroy graveyard so a
  // still-in-flight frame keeps the old buffer) and reuse across frames.
  frame_index_ = (frame_index_ + 1) % Device::kMaxFramesInFlight;
  FrameBuffers& rb = frames_[frame_index_];
  const u64 vtx_size = static_cast<u64>(draw_data->TotalVtxCount) * sizeof(ImDrawVert);
  const u64 idx_size = static_cast<u64>(draw_data->TotalIdxCount) * sizeof(ImDrawIdx);
  if (!rb.vertices || rb.vertices.size < vtx_size) {
    if (rb.vertices) device_->DestroyBufferDeferred(rb.vertices);
    rb.vertices = device_->CreateBuffer(std::max<u64>(vtx_size, 1), kBufferUsageVertex, true);
  }
  if (!rb.indices || rb.indices.size < idx_size) {
    if (rb.indices) device_->DestroyBufferDeferred(rb.indices);
    rb.indices = device_->CreateBuffer(std::max<u64>(idx_size, 1), kBufferUsageIndex, true);
  }
  if (!rb.vertices.mapped || !rb.indices.mapped) return;

  auto* vtx_dst = static_cast<ImDrawVert*>(rb.vertices.mapped);
  auto* idx_dst = static_cast<ImDrawIdx*>(rb.indices.mapped);
  for (const ImDrawList* list : draw_data->CmdLists) {
    std::memcpy(vtx_dst, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
    std::memcpy(idx_dst, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
    vtx_dst += list->VtxBuffer.Size;
    idx_dst += list->IdxBuffer.Size;
  }

  // Render state: pipeline, buffers, viewport and the ortho scale/translate the
  // vertex shader applies (imgui screen space -> clip space, no y-flip since
  // Vulkan clip y is already down).
  cmd.BindPipeline(pipeline_);
  cmd.BindVertexBuffer(0, rb.vertices, 0);
  cmd.BindIndexBuffer(rb.indices, 0, IndexType::kUint16);
  cmd.SetViewport(0, 0, static_cast<f32>(fb_width), static_cast<f32>(fb_height));
  ImGuiPush push;
  push.scale[0] = 2.0f / draw_data->DisplaySize.x;
  push.scale[1] = 2.0f / draw_data->DisplaySize.y;
  push.translate[0] = -1.0f - draw_data->DisplayPos.x * push.scale[0];
  push.translate[1] = -1.0f - draw_data->DisplayPos.y * push.scale[1];
  cmd.Push(push);

  const ImVec2 clip_off = draw_data->DisplayPos;
  const ImVec2 clip_scale = draw_data->FramebufferScale;
  u64 last_texid = 0;
  int global_vtx = 0, global_idx = 0;
  for (const ImDrawList* list : draw_data->CmdLists) {
    for (const ImDrawCmd& pcmd : list->CmdBuffer) {
      if (pcmd.UserCallback != nullptr) continue;  // user callbacks unsupported

      // Project the clip rectangle into framebuffer space and clamp it.
      f32 min_x = (pcmd.ClipRect.x - clip_off.x) * clip_scale.x;
      f32 min_y = (pcmd.ClipRect.y - clip_off.y) * clip_scale.y;
      f32 max_x = (pcmd.ClipRect.z - clip_off.x) * clip_scale.x;
      f32 max_y = (pcmd.ClipRect.w - clip_off.y) * clip_scale.y;
      min_x = std::max(min_x, 0.0f);
      min_y = std::max(min_y, 0.0f);
      max_x = std::min(max_x, static_cast<f32>(fb_width));
      max_y = std::min(max_y, static_cast<f32>(fb_height));
      if (max_x <= min_x || max_y <= min_y) continue;
      cmd.SetScissor(static_cast<i32>(min_x), static_cast<i32>(min_y),
                     static_cast<u32>(max_x - min_x), static_cast<u32>(max_y - min_y));

      // Bind the draw's texture (font atlas or user texture); the id is the
      // TextureView value stashed by UpdateTexture. Skip redundant rebinds.
      const u64 texid = static_cast<u64>(pcmd.GetTexID());
      if (texid != last_texid) {
        cmd.BindTransient(0, {Bind::Combined(0, TextureView{texid}, sampler_)});
        last_texid = texid;
      }

      cmd.DrawIndexed(pcmd.ElemCount, 1, pcmd.IdxOffset + global_idx,
                      static_cast<i32>(pcmd.VtxOffset + global_vtx), 0);
    }
    global_idx += list->IdxBuffer.Size;
    global_vtx += list->VtxBuffer.Size;
  }
}

void ImGuiRenderer::Shutdown() {
  if (!device_) return;
  // Free the texture backings we created (own copy of the list, so no global
  // ImGui:: call; DestroyTexture erases as it goes).
  while (!textures_.empty()) DestroyTexture(textures_.back());
  for (FrameBuffers& rb : frames_) {
    if (rb.vertices) device_->DestroyBuffer(rb.vertices);
    if (rb.indices) device_->DestroyBuffer(rb.indices);
    rb = {};
  }
  if (pipeline_) device_->DestroyPipeline(pipeline_);
  pipeline_ = {};
  device_ = nullptr;
}

}  // namespace rx::render
