#include "rxe/render/core/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/check.h>
#include <base/option.h>

#include "base/containers/span.h"
#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "base/time/time.h"
#include "foundation/algorithm/sort.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "foundation/memory/memory_tracker.h"
#include "rxe/asset/primitives.h"
#include "rxe/asset/texture_compress.h"
#include "rxe/asset/exr_write.h"
#include "shaders/debug_line_ps_hlsl.h"
#include "shaders/debug_line_vs_hlsl.h"
#include "shaders/pick_id_ps_hlsl.h"
#include "shaders/pick_id_vs_hlsl.h"

namespace rx::render {

// Editor debug lines

namespace {
// One vertex of a debug line: world position + packed rgba8 colour.
struct DebugLineVertex {
  f32 pos[3];
  u8 rgba[4];
};
struct DebugLinePush {
  Mat4 view_proj;
};
} // namespace

void Renderer::BuildDebugLinePipelines() {
  if (debug_line_pipeline_)
    return;
  gpu::VertexBufferLayout stream{
      .stride = sizeof(DebugLineVertex),
      .attributes = {
          {0, gpu::Format::kRGB32Float, offsetof(DebugLineVertex, pos)},
          {1, gpu::Format::kRGBA8Unorm, offsetof(DebugLineVertex, rgba)}}};
  gpu::GraphicsPipelineDesc desc{
      .vertex = RX_SHADER(k_debug_line_vs_hlsl),
      .fragment = RX_SHADER(k_debug_line_ps_hlsl),
      .vertex_buffers = {stream},
      .topology = gpu::PrimitiveTopology::kLineList,
      .raster = {.cull = gpu::CullMode::kNone,
                 .front = gpu::FrontFace::kCounterClockwise,
                 .polygon = gpu::PolygonMode::kFill},
      .depth = {.test = true,
                .write = false,
                .compare = gpu::CompareOp::kGreaterEqual,
                .format = kDepthFormat},
      .color_formats = {kSceneColorFormat},
      .blend = {gpu::BlendMode::kAlpha},
      .push_constant_size = gpu::PushSize<DebugLinePush>(),
      .debug_name = "debug_line",
  };
  debug_line_pipeline_ = device_->CreateGraphicsPipeline(desc);
  // Overlay variant: identical but no depth test, so it always draws on top.
  desc.depth = {.test = false, .write = false, .format = kDepthFormat};
  desc.debug_name = "debug_line_overlay";
  debug_line_overlay_pipeline_ = device_->CreateGraphicsPipeline(desc);
}

namespace {

// Built-in 4-wide, 6-tall uppercase stroke font for WorldText. Each glyph is a
// run of segments on that grid, emitted as camera-facing DebugLines (origin +
// right*x + up*y). Unlisted characters draw nothing.
void AppendGlyphBillboard(char c, f32 ox, f32 oy, f32 scale, const Vec3 &origin,
                          const Vec3 &right, const Vec3 &up, u32 rgba,
                          base::Vector<DebugLine> &out) {
  auto seg = [&](f32 x0, f32 y0, f32 x1, f32 y1) {
    out.push_back({origin + right * ((ox + x0) * scale) + up * ((oy + y0) * scale),
                   origin + right * ((ox + x1) * scale) + up * ((oy + y1) * scale), rgba});
  };
  switch (c) {
    case 'A': seg(0,0,2,6); seg(2,6,4,0); seg(1,2,3,2); break;
    case 'B': seg(0,0,0,6); seg(0,6,3,6); seg(3,6,3,3); seg(3,3,0,3); seg(3,3,3,0); seg(3,0,0,0); break;
    case 'C': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); break;
    case 'D': seg(0,0,0,6); seg(0,6,2,6); seg(2,6,4,4); seg(4,4,4,2); seg(4,2,2,0); seg(2,0,0,0); break;
    case 'E': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); seg(0,3,3,3); break;
    case 'F': seg(4,6,0,6); seg(0,6,0,0); seg(0,3,3,3); break;
    case 'G': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); seg(4,0,4,3); seg(4,3,2,3); break;
    case 'H': seg(0,0,0,6); seg(4,0,4,6); seg(0,3,4,3); break;
    case 'I': seg(2,0,2,6); seg(1,6,3,6); seg(1,0,3,0); break;
    case 'J': seg(4,6,4,1); seg(4,1,3,0); seg(3,0,1,0); seg(1,0,0,1); break;
    case 'K': seg(0,0,0,6); seg(0,3,4,6); seg(0,3,4,0); break;
    case 'L': seg(0,6,0,0); seg(0,0,4,0); break;
    case 'M': seg(0,0,0,6); seg(0,6,2,3); seg(2,3,4,6); seg(4,6,4,0); break;
    case 'N': seg(0,0,0,6); seg(0,6,4,0); seg(4,0,4,6); break;
    case 'O': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); break;
    case 'P': seg(0,0,0,6); seg(0,6,3,6); seg(3,6,3,3); seg(3,3,0,3); break;
    case 'Q': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(2,2,4,0); break;
    case 'R': seg(0,0,0,6); seg(0,6,3,6); seg(3,6,3,3); seg(3,3,0,3); seg(1,3,4,0); break;
    case 'S': seg(4,6,0,6); seg(0,6,0,3); seg(0,3,4,3); seg(4,3,4,0); seg(4,0,0,0); break;
    case 'T': seg(0,6,4,6); seg(2,6,2,0); break;
    case 'U': seg(0,6,0,0); seg(0,0,4,0); seg(4,0,4,6); break;
    case 'V': seg(0,6,2,0); seg(2,0,4,6); break;
    case 'W': seg(0,6,1,0); seg(1,0,2,3); seg(2,3,3,0); seg(3,0,4,6); break;
    case 'X': seg(0,0,4,6); seg(0,6,4,0); break;
    case 'Y': seg(0,6,2,3); seg(4,6,2,3); seg(2,3,2,0); break;
    case 'Z': seg(0,6,4,6); seg(4,6,0,0); seg(0,0,4,0); break;
    case '0': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(0,0,4,6); break;
    case '1': seg(1,4,2,6); seg(2,6,2,0); seg(1,0,3,0); break;
    case '2': seg(0,6,4,6); seg(4,6,4,3); seg(4,3,0,3); seg(0,3,0,0); seg(0,0,4,0); break;
    case '3': seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(1,3,4,3); break;
    case '4': seg(0,6,0,3); seg(0,3,4,3); seg(3,6,3,0); break;
    case '5': seg(4,6,0,6); seg(0,6,0,3); seg(0,3,4,3); seg(4,3,4,0); seg(4,0,0,0); break;
    case '6': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); seg(4,0,4,3); seg(4,3,0,3); break;
    case '7': seg(0,6,4,6); seg(4,6,1,0); break;
    case '8': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(0,3,4,3); break;
    case '9': seg(4,0,4,6); seg(4,6,0,6); seg(0,6,0,3); seg(0,3,4,3); break;
    case '+': seg(2,1,2,5); seg(0,3,4,3); break;
    case '-': seg(0,3,4,3); break;
    case '.': seg(1,0,2,0); break;
    case '/': seg(0,0,4,6); break;
    case ':': seg(2,1,2,2); seg(2,4,2,5); break;
    default: break;
  }
}

void TessellateWorldText(const WorldText &t, const Vec3 &right, const Vec3 &up,
                         base::Vector<DebugLine> &out) {
  const f32 scale = t.size / 6.0f;
  const f32 advance = 5.0f;  // grid units per glyph cell
  const f32 line_h = 8.0f;   // grid units per line
  size_t line_index = 0;
  size_t start = 0;
  const base::String &s = t.text;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i != s.size() && s[i] != '\n')
      continue;
    const size_t len = i - start;
    const f32 line_width = advance * static_cast<f32>(len);
    const f32 ox0 = -t.align * line_width;
    const f32 oy = -static_cast<f32>(line_index) * line_h;
    for (size_t j = 0; j < len; ++j) {
      char c = s[start + j];
      if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 32);
      AppendGlyphBillboard(c, ox0 + static_cast<f32>(j) * advance, oy, scale, t.position, right, up,
                           t.rgba, out);
    }
    ++line_index;
    start = i + 1;
  }
}

}  // namespace

void Renderer::DrawDebugLines(gpu::CommandList &cmd, const FrameView &view,
                              const Mat4 &view_proj, gpu::Extent2D extent) {
  // Camera-facing basis so WorldText billboards stay upright and readable.
  Vec3 text_right = Cross(Normalize(view.camera.target - view.camera.eye), Vec3{0, 1, 0});
  if (Length(text_right) < 1e-4f)
    text_right = Vec3{1, 0, 0};
  text_right = Normalize(text_right);
  const Vec3 text_up = Normalize(Cross(text_right, Normalize(view.camera.target - view.camera.eye)));
  base::Vector<DebugLine> text_depth, text_overlay;
  for (const WorldText &t : view.world_texts)
    TessellateWorldText(t, text_right, text_up, t.overlay ? text_overlay : text_depth);

  const size_t total = view.debug_lines.size() + view.debug_lines_overlay.size() +
                       text_depth.size() + text_overlay.size();
  if (total == 0)
    return;
  BuildDebugLinePipelines();
  if (!debug_line_pipeline_ || !debug_line_overlay_pipeline_)
    return;

  const u32 slot = frame_index_ % kFramesInFlight;
  const u32 needed_vertices = static_cast<u32>(total * 2);
  if (debug_line_vbo_capacity_[slot] < needed_vertices) {
    // Grow to the requested count (host-visible; the slot's fence already fired
    // in BeginFrame, so overwriting is safe). Round up to reduce churn.
    u32 cap = 256;
    while (cap < needed_vertices)
      cap *= 2;
    device_->DestroyBufferDeferred(debug_line_vbo_[slot]);
    debug_line_vbo_[slot] =
        device_->CreateBuffer(static_cast<u64>(cap) * sizeof(DebugLineVertex),
                              gpu::kBufferUsageVertex, /*host_visible=*/true);
    debug_line_vbo_capacity_[slot] = cap;
  }
  auto *verts = static_cast<DebugLineVertex *>(debug_line_vbo_[slot].mapped);
  if (!verts)
    return;

  auto append = [&](const DebugLine &l, u32 &at) {
    auto write = [&](const Vec3 &p) {
      verts[at].pos[0] = p.x;
      verts[at].pos[1] = p.y;
      verts[at].pos[2] = p.z;
      verts[at].rgba[0] = static_cast<u8>((l.rgba >> 24) & 0xff);
      verts[at].rgba[1] = static_cast<u8>((l.rgba >> 16) & 0xff);
      verts[at].rgba[2] = static_cast<u8>((l.rgba >> 8) & 0xff);
      verts[at].rgba[3] = static_cast<u8>(l.rgba & 0xff);
      ++at;
    };
    write(l.a);
    write(l.b);
  };

  u32 count = 0;
  const u32 depth_first = count;
  for (const DebugLine &l : view.debug_lines)
    append(l, count);
  for (const DebugLine &l : text_depth)
    append(l, count);
  const u32 depth_count = count - depth_first;
  const u32 overlay_first = count;
  for (const DebugLine &l : view.debug_lines_overlay)
    append(l, count);
  for (const DebugLine &l : text_overlay)
    append(l, count);
  const u32 overlay_count = count - overlay_first;

  DebugLinePush push{view_proj};
  cmd.SetViewport(0, 0, static_cast<f32>(extent.width),
                  static_cast<f32>(extent.height));
  cmd.SetScissor(0, 0, extent.width, extent.height);
  cmd.BindVertexBuffer(0, debug_line_vbo_[slot], 0);
  if (depth_count) {
    cmd.BindPipeline(debug_line_pipeline_);
    cmd.PushConstants(&push, sizeof(push), 0);
    cmd.Draw(depth_count, 1, depth_first, 0);
  }
  if (overlay_count) {
    cmd.BindPipeline(debug_line_overlay_pipeline_);
    cmd.PushConstants(&push, sizeof(push), 0);
    cmd.Draw(overlay_count, 1, overlay_first, 0);
  }
}

// Editor picking

void Renderer::RequestPick(u32 x, u32 y) {
  pick_requested_ = true;
  pick_x_ = x;
  pick_y_ = y;
}

base::Optional<PickResult> Renderer::TakePickResult() {
  if (!pick_result_ready_)
    return base::nullopt;
  pick_result_ready_ = false;
  return PickResult{pick_result_id_};
}

namespace {
struct PickPush {
  Mat4 mvp;
  u32 id;
};
} // namespace

void Renderer::RenderPickPass(const FrameView &view) {
  pick_requested_ = false;
  if (!device_ || device_->is_stub() || render_width_ == 0 ||
      render_height_ == 0)
    return;

  // (Re)create the id + depth targets at render resolution.
  if (pick_image_w_ != render_width_ || pick_image_h_ != render_height_ ||
      !pick_id_image_) {
    if (pick_id_image_)
      device_->DestroyImageDeferred(pick_id_image_);
    if (pick_depth_image_)
      device_->DestroyImageDeferred(pick_depth_image_);
    pick_id_image_ = device_->CreateImage2D(
        gpu::Format::kR32Uint, {render_width_, render_height_},
        gpu::kTextureUsageColorTarget | gpu::kTextureUsageTransferSrc);
    pick_depth_image_ = device_->CreateImage2D(gpu::Format::kD32Float,
                                               {render_width_, render_height_},
                                               gpu::kTextureUsageDepthTarget);
    pick_image_w_ = render_width_;
    pick_image_h_ = render_height_;
  }
  if (!pick_id_image_ || !pick_depth_image_)
    return;

  if (!pick_pipeline_) {
    pick_pipeline_ = device_->CreateGraphicsPipeline({
        .vertex = RX_SHADER(k_pick_id_vs_hlsl),
        .fragment = RX_SHADER(k_pick_id_ps_hlsl),
        .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                            .attributes = {{0, gpu::Format::kRGB32Float,
                                            offsetof(asset::Vertex,
                                                     position)}}}},
        .raster = {.cull = gpu::CullMode::kBack,
                   .front = gpu::FrontFace::kCounterClockwise},
        .depth = {.test = true,
                  .write = true,
                  .compare = gpu::CompareOp::kGreaterEqual,
                  .format = gpu::Format::kD32Float},
        .color_formats = {gpu::Format::kR32Uint},
        .push_constant_size = gpu::PushSize<PickPush>(),
        .debug_name = "pick_id",
    });
  }
  if (!pick_pipeline_)
    return;

  const f32 aspect =
      static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  const Mat4 view_proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f) *
                         LookAt(view.camera.eye, view.camera.target, {0, 1, 0});

  device_->ImmediateSubmit([&](gpu::CommandList &cmd) {
    cmd.Barrier(gpu::Transition(pick_id_image_, gpu::ResourceState::kUndefined,
                           gpu::ResourceState::kColorTarget));
    cmd.Barrier(gpu::Transition(pick_depth_image_, gpu::ResourceState::kUndefined,
                           gpu::ResourceState::kDepthTarget));
    gpu::ColorAttachment color{.view = pick_id_image_.view, .load = gpu::LoadOp::kClear};
    gpu::DepthAttachment depth{
        .view = pick_depth_image_.view, .load = gpu::LoadOp::kClear, .clear = 0.0f};
    cmd.BeginRendering({.extent = {render_width_, render_height_},
                        .colors = base::Span(&color, 1),
                        .depth = &depth});
    cmd.SetViewport(0, 0, static_cast<f32>(render_width_),
                    static_cast<f32>(render_height_));
    cmd.SetScissor(0, 0, render_width_, render_height_);
    cmd.BindPipeline(pick_pipeline_);
    for (const DrawItem &item : view.draws) {
      if (item.pick_id == 0)
        continue; // unpickable: leave the cleared 0
      const gpu::GpuMesh *mesh = meshes_.find(item.mesh);
      if (!mesh || !mesh->indices)
        continue;
      PickPush push{view_proj * item.transform, item.pick_id};
      cmd.PushConstants(&push, sizeof(push), 0);
      cmd.BindVertexBuffer(0, mesh->vertices, 0);
      cmd.BindIndexBuffer(mesh->indices, 0, gpu::IndexType::kUint32);
      for (const gpu::GpuSubmesh &submesh : mesh->submeshes) {
        cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
      }
    }
    cmd.EndRendering();
  });

  // Read back the whole id target and sample the requested pixel (in output
  // pixels, scaled to render resolution).
  base::Vector<u32> pixels(static_cast<size_t>(render_width_) * render_height_);
  if (!device_->ReadbackImage(pick_id_image_, gpu::ResourceState::kColorTarget,
                              pixels.data(), pixels.size() * sizeof(u32))) {
    return;
  }
  u32 px = pick_x_, py = pick_y_;
  if (output_width_ > 0 && output_height_ > 0) {
    px = pick_x_ * render_width_ / output_width_;
    py = pick_y_ * render_height_ / output_height_;
  }
  if (px >= render_width_)
    px = render_width_ - 1;
  if (py >= render_height_)
    py = render_height_ - 1;
  pick_result_id_ = pixels[static_cast<size_t>(py) * render_width_ + px];
  pick_result_ready_ = true;
}

} // namespace rx::render
