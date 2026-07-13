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
  u32 control[4];      // ring index, phase, fft flag, disturbance count
};
static_assert(sizeof(WaterFieldPush) == 64);

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
                          {3, BindingType::kStorageBuffer}}}},
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
    TextureBarrier to_general[kRingCount * 2];
    u32 n = 0;
    for (u32 r = 0; r < kRingCount; ++r)
      for (u32 p = 0; p < 2; ++p)
        to_general[n++] =
            Transition(rings_[r][p], ResourceState::kUndefined, ResourceState::kGeneral);
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
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    device.DestroyBuffer(params_[f]);
    device.DestroyBuffer(disturbances_[f]);
  }
}

void WaterField::AddToGraph(RenderGraph& graph, const UpdateParams& params,
                            TextureView ocean_normal_foam) {
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
  graph.AddPass(
      "water_field", [](RenderGraph::PassBuilder&) {},
      [this, params, ocean_normal_foam, prev_origin, texel_world, slot, write, fft,
       disturbance_count](PassContext& ctx) {
        (void)texel_world;
        // The FFT ocean writes its foam map just before us; make those writes
        // visible to our sampled read.
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        ctx.cmd->BindPipeline(pipeline_);

        f32 inv = kDriftSpeed / std::sqrt(kDriftDirX * kDriftDirX + kDriftDirZ * kDriftDirZ);
        f32 drift_x = kDriftDirX * inv;
        f32 drift_z = kDriftDirZ * inv;

        for (u32 r = 0; r < kRingCount; ++r) {
          const GpuImage& cur = rings_[r][write];
          const GpuImage& prev = rings_[r][write ^ 1u];
          TextureView foam_view = ocean_normal_foam ? ocean_normal_foam : cur.view;

          ctx.cmd->BindTransient(
              0, {InGeneral(Bind::Combined(0, prev.view, sampler_)),
                  Bind::StorageView(1, cur.view),
                  InGeneral(Bind::Combined(2, foam_view, sampler_)),
                  Bind::StorageBuffer(3, disturbances_[slot])});

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
          push.control[2] = fft ? 1u : 0u;
          push.control[3] = disturbance_count;

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
