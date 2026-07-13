#include "render/geometry/water_field.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

#include "core/log.h"
#include "shaders/water_field_cs_hlsl.h"

namespace rx::render {
namespace {

struct WaterFieldPush {
  f32 origin[4];       // new origin xz, half_extent, texel_world
  f32 prev_origin[4];  // old origin xz (advection source frame)
  f32 drift_time[4];   // wave-drift xz, dt, time
  u32 control[4];      // ring index, phase, flags, disturbance count
  Mat4 view_proj;      // world -> clip (project the texel column to screen)
  Mat4 inv_view_proj;  // clip -> world (reconstruct geometry from depth)
  f32 island[4];       // center xz, sigma, peak (obstacle terrain)
  f32 idepth0[4];      // near_plane, band, foam_scale, water_level
  f32 idepth1[4];      // render_w, render_h, xz_proximity, ripple_gain
};
static_assert(sizeof(WaterFieldPush) == 240);

// Interaction flag bits packed into control.z.
constexpr u32 kFlagFftFoam = 1u;
constexpr u32 kFlagInteract = 2u;
constexpr u32 kFlagObstacle = 4u;

// Waterline detection band + tuning for the depth-driven ripple/foam impulse.
// The impulse is driven by the per-frame CHANGE of the intersection band (which
// stays alive at steady state because the tested waterline rides the swell), so
// it is zero-mean over a wave cycle: the gain can be generous without the field
// accumulating a DC drift.
constexpr f32 kWaterlineBand = 0.8f;   // meters above/below the surface the band spans
constexpr f32 kInteractProximity = 2.0f;  // geometry must sit within this of the column
constexpr f32 kInteractRippleGain = 45.0f;  // ripple velocity per unit band change
constexpr f32 kInteractFoamScale = 0.25f;   // foam RATE per unit swell speed at the waterline
// Bounded standing dent the footprint pins the surface toward (a spring target,
// so it cannot accumulate); the wave equation rings it outward for the visible
// persistent ripple at the waterline.
constexpr f32 kInteractStandDisp = 0.35f;   // meters the footprint depresses the surface

struct GpuDisturbance {
  f32 pos_radius[4];
  f32 params[4];
};
static_assert(sizeof(GpuDisturbance) == sizeof(WaterDisturbance));

// Dominant wave drift the foam advects along; mirrors the FFT ocean wind
// (0.8, 0.6) at a phase-speed-scaled magnitude, and matches the Gerstner
// crest directions used when the FFT ocean is off.
constexpr f32 kDriftDirX = 0.8f;
constexpr f32 kDriftDirZ = 0.6f;
constexpr f32 kDriftSpeed = 1.6f;  // m/s

}  // namespace

bool WaterField::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_water_field_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kCombinedTextureSampler},
                          {6, BindingType::kStorageImage},
                          {7, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(WaterFieldPush),
      .debug_name = "water_field",
  });
  if (!pipeline_) return false;

  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .address_u = AddressMode::kClampToEdge,
                                .address_v = AddressMode::kClampToEdge,
                                .address_w = AddressMode::kClampToEdge});

  for (u32 r = 0; r < kRingCount; ++r) {
    for (u32 p = 0; p < 2; ++p) {
      rings_[r][p] = device.CreateImage2D(Format::kRGBA16Float, {kSize, kSize},
                                          kTextureUsageStorage | kTextureUsageSampled);
      if (!rings_[r][p]) {
        RX_WARN("water field allocation failed; foam field disabled");
        Destroy(device);
        return false;
      }
    }
  }
  // Ring-0 waterline intersection band, ping-ponged with the rings (recenters
  // with the camera). R32F: storage-writable everywhere and enough range for a
  // 0..1 mask. A no-op unless local interaction is enabled.
  for (u32 p = 0; p < 2; ++p) {
    mask_[p] = device.CreateImage2D(Format::kR32Float, {kSize, kSize},
                                    kTextureUsageStorage | kTextureUsageSampled);
    if (!mask_[p]) {
      RX_WARN("water field mask allocation failed; foam field disabled");
      Destroy(device);
      return false;
    }
  }
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    params_[f] = device.CreateBuffer(sizeof(GpuParams), kBufferUsageUniform, true);
    disturbances_[f] = device.CreateBuffer(kMaxDisturbances * sizeof(GpuDisturbance),
                                           kBufferUsageStorage, true);
    if (!params_[f].mapped || !disturbances_[f].mapped) {
      RX_WARN("water field buffer mapping failed; foam field disabled");
      Destroy(device);
      return false;
    }
  }

  // The ping-pong rings live in GENERAL, like the FFT ocean maps: compute keeps
  // writing them and the water shader samples them straight out of GENERAL.
  device.ImmediateSubmit([&](CommandList& cmd) {
    TextureBarrier to_general[kRingCount * 2 + 2];
    u32 n = 0;
    for (u32 r = 0; r < kRingCount; ++r)
      for (u32 p = 0; p < 2; ++p)
        to_general[n++] =
            Transition(rings_[r][p], ResourceState::kUndefined, ResourceState::kGeneral);
    for (u32 p = 0; p < 2; ++p)
      to_general[n++] = Transition(mask_[p], ResourceState::kUndefined, ResourceState::kGeneral);
    cmd.TextureBarriers(std::span<const TextureBarrier>(to_general, n));
  });
  return true;
}

void WaterField::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (u32 r = 0; r < kRingCount; ++r)
    for (u32 p = 0; p < 2; ++p) {
      if (rings_[r][p]) device.DestroyImage(rings_[r][p]);
      rings_[r][p] = {};
    }
  for (u32 p = 0; p < 2; ++p) {
    if (mask_[p]) device.DestroyImage(mask_[p]);
    mask_[p] = {};
  }
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    device.DestroyBuffer(params_[f]);
    device.DestroyBuffer(disturbances_[f]);
  }
}

void WaterField::AddToGraph(RenderGraph& graph, const UpdateParams& params,
                            TextureView ocean_normal_foam, TextureView ocean_displacement,
                            ResourceHandle opaque_depth) {
  if (!available()) return;

  // Snap each ring's origin to its own texel grid so the field does not swim
  // as the camera moves, and remember the previous origin to resample from.
  f32 prev_origin[kRingCount][2];
  f32 texel_world[kRingCount];
  for (u32 r = 0; r < kRingCount; ++r) {
    prev_origin[r][0] = origin_[r][0];
    prev_origin[r][1] = origin_[r][1];
    texel_world[r] = 2.0f * kRingHalfExtent[r] / static_cast<f32>(kSize);
    origin_[r][0] = std::round(params.camera_pos.x / texel_world[r]) * texel_world[r];
    origin_[r][1] = std::round(params.camera_pos.z / texel_world[r]) * texel_world[r];
    if (!centered_) {  // first frame: no history, resample lands out of bounds
      prev_origin[r][0] = origin_[r][0] + 1e6f;
      prev_origin[r][1] = origin_[r][1] + 1e6f;
    }
  }
  centered_ = true;
  write_ ^= 1u;  // this frame writes the freshly flipped buffer, reads the other

  const u32 slot = params.frame_slot % Device::kMaxFramesInFlight;

  // Params CB the water shader uses to map world XZ into each ring.
  GpuParams gp{};
  for (u32 r = 0; r < kRingCount; ++r) {
    gp.ring[r][0] = origin_[r][0];
    gp.ring[r][1] = origin_[r][1];
    gp.ring[r][2] = kRingHalfExtent[r];
    gp.ring[r][3] = texel_world[r];
  }
  std::memcpy(params_[slot].mapped, &gp, sizeof(gp));

  // Object disturbances for this frame (bounded).
  u32 disturbance_count = std::min(params.disturbance_count, kMaxDisturbances);
  if (disturbance_count > 0 && params.disturbances) {
    std::memcpy(disturbances_[slot].mapped, params.disturbances,
                disturbance_count * sizeof(GpuDisturbance));
  }

  const u32 write = write_;
  const bool fft = params.fft_ocean && static_cast<bool>(ocean_normal_foam);
  // Interaction reads the opaque prepass depth; the caller schedules this pass
  // after the prepass writes it (see renderer.cc). Depth is always valid while
  // the field is active, so we always transition/bind it and let the flag gate
  // whether the shader actually reads it.
  const bool interact = params.interaction && opaque_depth != kInvalidResource;
  u32 flags = (fft ? kFlagFftFoam : 0u) | (interact ? kFlagInteract : 0u) |
              (params.obstacle ? kFlagObstacle : 0u);
  graph.AddPass(
      "water_field",
      [opaque_depth](RenderGraph::PassBuilder& b) {
        if (opaque_depth != kInvalidResource)
          b.Read(opaque_depth, ResourceUsage::kSampledCompute);
      },
      [this, params, ocean_normal_foam, ocean_displacement, prev_origin, texel_world, slot, write,
       flags, disturbance_count, opaque_depth](PassContext& ctx) {
        (void)texel_world;
        // The FFT ocean writes its foam map just before us; make those writes
        // visible to our sampled read.
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        ctx.cmd->BindPipeline(pipeline_);

        f32 inv = kDriftSpeed / std::sqrt(kDriftDirX * kDriftDirX + kDriftDirZ * kDriftDirZ);
        f32 drift_x = kDriftDirX * inv;
        f32 drift_z = kDriftDirZ * inv;
        // Depth for slot 4: the real prepass depth when available, else the ring
        // itself as a harmless in-layout placeholder (the flag keeps it unread).
        TextureView depth_view =
            (opaque_depth != kInvalidResource) ? ctx.graph->image(opaque_depth).view : TextureView{};

        for (u32 r = 0; r < kRingCount; ++r) {
          const GpuImage& cur = rings_[r][write];
          const GpuImage& prev = rings_[r][write ^ 1u];
          TextureView foam_view = ocean_normal_foam ? ocean_normal_foam : cur.view;
          TextureView disp_view = ocean_displacement ? ocean_displacement : cur.view;
          TextureView slot4 = depth_view ? depth_view : cur.view;

          ctx.cmd->BindTransient(
              0, {InGeneral(Bind::Combined(0, prev.view, sampler_)),
                  Bind::StorageView(1, cur.view),
                  InGeneral(Bind::Combined(2, foam_view, sampler_)),
                  Bind::StorageBuffer(3, disturbances_[slot]),
                  depth_view ? Bind::SampledView(4, slot4) : InGeneral(Bind::SampledView(4, slot4)),
                  InGeneral(Bind::Combined(5, mask_[write ^ 1u].view, sampler_)),
                  Bind::StorageView(6, mask_[write].view),
                  InGeneral(Bind::Combined(7, disp_view, sampler_))});

          WaterFieldPush push{};
          push.origin[0] = origin_[r][0];
          push.origin[1] = origin_[r][1];
          push.origin[2] = kRingHalfExtent[r];
          push.origin[3] = texel_world[r];
          push.prev_origin[0] = prev_origin[r][0];
          push.prev_origin[1] = prev_origin[r][1];
          push.drift_time[0] = drift_x;
          push.drift_time[1] = drift_z;
          push.drift_time[2] = params.dt;
          push.drift_time[3] = params.time;
          push.control[0] = r;
          push.control[2] = flags;
          push.control[3] = disturbance_count;
          push.view_proj = params.view_proj;
          push.inv_view_proj = params.inv_view_proj;
          push.island[0] = params.island[0];
          push.island[1] = params.island[1];
          push.island[2] = params.island[2];
          push.island[3] = params.island[3];
          push.idepth0[0] = kInteractStandDisp;
          push.idepth0[1] = kWaterlineBand;
          push.idepth0[2] = kInteractFoamScale;
          push.idepth0[3] = params.water_level;
          push.idepth1[0] = params.render_size[0];
          push.idepth1[1] = params.render_size[1];
          push.idepth1[2] = kInteractProximity;
          push.idepth1[3] = kInteractRippleGain;

          const u32 groups = kSize / 8;
          push.control[1] = 0u;  // recenter + advect + decay + ripple step
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch(groups, groups, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

          push.control[1] = 1u;  // crest foam + object disturbance injection
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch(groups, groups, 1);
          // The rings are sampled by the water pixel shader downstream.
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        }
      });
}

}  // namespace rx::render
