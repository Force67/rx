#include "rxe/render/atmosphere/precip_occlusion.h"

#include <math.h>

#include "base/functional/function.h"
#include "foundation/logging/log.h"

namespace rx::render {

bool PrecipOcclusion::Initialize(gpu::Device& device) {
  map_ = device.CreateImage2D(
      kFormat, {kResolution, kResolution},
      gpu::kTextureUsageDepthTarget | gpu::kTextureUsageSampled | gpu::kTextureUsageTransferDst);
  sampler_ = device.GetSampler({.min_filter = gpu::Filter::kLinear,
                                .mag_filter = gpu::Filter::kLinear,
                                .address_u = gpu::AddressMode::kClampToEdge,
                                .address_v = gpu::AddressMode::kClampToEdge});
  if (!map_ || !sampler_) {
    RX_WARN("precipitation occlusion map allocation failed; feature disabled");
    Destroy(device);
    return false;
  }
  // Park the map shader-readable so consumers can bind it before the first
  // render (the far-plane clear reads as "open sky" only after one render;
  // until then dirty_ forces one on the first active frame anyway).
  device.ImmediateSubmit([&](gpu::CommandList& cmd) {
    cmd.Barrier(gpu::Transition(map_, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst));
    cmd.ClearDepth(map_, 1.0f);
    cmd.Barrier(gpu::Transition(map_, gpu::ResourceState::kCopyDst, gpu::ResourceState::kShaderReadAll));
  });
  return true;
}

void PrecipOcclusion::Destroy(gpu::Device& device) {
  if (map_) device.DestroyImage(map_);
  map_ = {};
  rendered_ = false;
  dirty_ = true;
}

void PrecipOcclusion::BeginFrame(const Vec3& eye, u32 frame_index) {
  // Quantize the anchor to the coarse cell: the projection window only ever
  // moves in whole cells (which are whole texels), so map content never
  // shimmers under camera motion; between cells the map is simply reused.
  f32 ax = ::floorf(eye.x / kAnchorCell + 0.5f) * kAnchorCell;
  f32 ay = ::floorf(eye.y / kAnchorCell + 0.5f) * kAnchorCell;
  f32 az = ::floorf(eye.z / kAnchorCell + 0.5f) * kAnchorCell;
  if (ax != center_[0] || ay != center_[1] || az != center_[2]) dirty_ = true;
  // Cheap steady-state refresh so doors/moving cover eventually update.
  if (frame_index % kRefreshFrames == 0) dirty_ = true;
  center_[0] = ax;
  center_[1] = ay;
  center_[2] = az;
}

void PrecipOcclusion::Params(f32 out[4]) const {
  out[0] = center_[0];
  out[1] = center_[2];
  out[2] = 1.0f / kHalfExtent;
  out[3] = center_[1] + kHalfHeight;  // top_y: depth 0 plane, high above the eye
}

void PrecipOcclusion::AddToGraph(RenderGraph& graph,
                                 const base::Function<void(gpu::CommandList&, const Mat4&)>& draw) {
  if (!available() || !dirty_) return;
  dirty_ = false;

  // Top-down orthographic world -> clip, built directly so the shader-side
  // uv/height decode in the header is exact by construction:
  //   clip.x = (x - cx) / kHalfExtent
  //   clip.y = (z - cz) / kHalfExtent
  //   clip.z = (top_y - y) / y_range()   (0 at top_y, 1 at the bottom)
  // Handedness does not matter: the caster pipelines cull nothing.
  Mat4 vp{};
  const f32 inv_r = 1.0f / kHalfExtent;
  const f32 inv_range = 1.0f / y_range();
  const f32 top_y = center_[1] + kHalfHeight;
  vp.m[0] = inv_r;                    // clip.x <- world.x
  vp.m[9] = inv_r;                    // clip.y <- world.z
  vp.m[6] = -inv_range;               // clip.z <- -world.y
  vp.m[12] = -center_[0] * inv_r;
  vp.m[13] = -center_[2] * inv_r;
  vp.m[14] = top_y * inv_range;
  vp.m[15] = 1.0f;

  graph.AddPass(
      "precip_occlusion", [](RenderGraph::PassBuilder&) {},
      [this, vp, draw](PassContext& ctx) {
        // Persistent map: shader-read between renders, depth target while
        // writing (same manual-barrier pattern as the local shadow atlas).
        gpu::TextureBarrier to_write = gpu::Transition(
            map_, rendered_ ? gpu::ResourceState::kShaderReadAll : gpu::ResourceState::kUndefined,
            gpu::ResourceState::kDepthTarget);
        rendered_ = true;
        ctx.cmd->TextureBarriers(base::Span(&to_write, 1));

        gpu::DepthAttachment depth{
            .view = map_.view, .load = gpu::LoadOp::kClear, .store = gpu::StoreOp::kStore, .clear = 1.0f};
        ctx.cmd->BeginRendering({.extent = {kResolution, kResolution}, .depth = &depth});
        ctx.cmd->SetViewport(0.0f, 0.0f, static_cast<f32>(kResolution),
                             static_cast<f32>(kResolution));
        ctx.cmd->SetScissor(0, 0, kResolution, kResolution);
        draw(*ctx.cmd, vp);
        ctx.cmd->EndRendering();

        // Sampled by the precipitation vertex stages and the surface-weather
        // compute; kShaderReadAll covers both.
        gpu::TextureBarrier to_read =
            gpu::Transition(map_, gpu::ResourceState::kDepthTarget, gpu::ResourceState::kShaderReadAll);
        ctx.cmd->TextureBarriers(base::Span(&to_read, 1));
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kAllCommands, gpu::BarrierScope::kComputeRead);
      });
}

}  // namespace rx::render
