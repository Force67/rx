#include "render/atmosphere/precip_occlusion.h"

#include <cmath>

#include "core/log.h"

namespace rx::render {

bool PrecipOcclusion::Initialize(Device& device) {
  map_ = device.CreateImage2D(
      kFormat, {kResolution, kResolution},
      kTextureUsageDepthTarget | kTextureUsageSampled | kTextureUsageTransferDst);
  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge});
  if (!map_ || !sampler_) {
    RX_WARN("precipitation occlusion map allocation failed; feature disabled");
    Destroy(device);
    return false;
  }
  // Park the map shader-readable so consumers can bind it before the first
  // render (the far-plane clear reads as "open sky" only after one render;
  // until then dirty_ forces one on the first active frame anyway).
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(map_, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.ClearDepth(map_, 1.0f);
    cmd.Barrier(Transition(map_, ResourceState::kCopyDst, ResourceState::kShaderReadAll));
  });
  return true;
}

void PrecipOcclusion::Destroy(Device& device) {
  if (map_) device.DestroyImage(map_);
  map_ = {};
  rendered_ = false;
  dirty_ = true;
}

void PrecipOcclusion::BeginFrame(const Vec3& eye, u32 frame_index) {
  // Quantize the anchor to the coarse cell: the projection window only ever
  // moves in whole cells (which are whole texels), so map content never
  // shimmers under camera motion; between cells the map is simply reused.
  f32 ax = std::floor(eye.x / kAnchorCell + 0.5f) * kAnchorCell;
  f32 ay = std::floor(eye.y / kAnchorCell + 0.5f) * kAnchorCell;
  f32 az = std::floor(eye.z / kAnchorCell + 0.5f) * kAnchorCell;
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
                                 const std::function<void(CommandList&, const Mat4&)>& draw) {
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
        TextureBarrier to_write = Transition(
            map_, rendered_ ? ResourceState::kShaderReadAll : ResourceState::kUndefined,
            ResourceState::kDepthTarget);
        rendered_ = true;
        ctx.cmd->TextureBarriers({&to_write, 1});

        DepthAttachment depth{
            .view = map_.view, .load = LoadOp::kClear, .store = StoreOp::kStore, .clear = 1.0f};
        ctx.cmd->BeginRendering({.extent = {kResolution, kResolution}, .depth = &depth});
        ctx.cmd->SetViewport(0.0f, 0.0f, static_cast<f32>(kResolution),
                             static_cast<f32>(kResolution));
        ctx.cmd->SetScissor(0, 0, kResolution, kResolution);
        draw(*ctx.cmd, vp);
        ctx.cmd->EndRendering();

        // Sampled by the precipitation vertex stages and the surface-weather
        // compute; kShaderReadAll covers both.
        TextureBarrier to_read =
            Transition(map_, ResourceState::kDepthTarget, ResourceState::kShaderReadAll);
        ctx.cmd->TextureBarriers({&to_read, 1});
        ctx.cmd->MemoryBarrier(BarrierScope::kAllCommands, BarrierScope::kComputeRead);
      });
}

}  // namespace rx::render
