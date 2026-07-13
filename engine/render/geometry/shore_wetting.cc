#include "render/geometry/shore_wetting.h"

#include <cmath>

#include "core/log.h"
#include "shaders/shore_wetting_cs_hlsl.h"

namespace rx::render {
namespace {

struct ShorePush {
  f32 field[4];       // origin xz, extent (m), 1/extent
  f32 prev_field[4];  // previous origin xz, unused zw
  f32 island[4];      // center xz, gaussian sigma (m), peak (m)
  f32 params[4];      // time, dt, drying time, fft flag
};
static_assert(sizeof(ShorePush) == 64);

}  // namespace

bool ShoreWetting::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_shore_wetting_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kCombinedTextureSampler},
                          {2, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(ShorePush),
      .debug_name = "shore_wetting",
  });
  if (!pipeline_) return false;

  linear_clamp_ = device.GetSampler({.min_filter = Filter::kLinear,
                                     .mag_filter = Filter::kLinear,
                                     .address_u = AddressMode::kClampToEdge,
                                     .address_v = AddressMode::kClampToEdge});
  linear_wrap_ = device.GetSampler({.min_filter = Filter::kLinear,
                                    .mag_filter = Filter::kLinear,
                                    .address_u = AddressMode::kRepeat,
                                    .address_v = AddressMode::kRepeat});

  const TextureUsageFlags usage =
      kTextureUsageSampled | kTextureUsageStorage | kTextureUsageTransferDst;
  for (GpuImage& field : fields_) {
    field = device.CreateImage2D(Format::kR16Float, {kResolution, kResolution}, usage);
  }
  dummy_ocean_ = device.CreateImage2D(Format::kRGBA16Float, {1, 1},
                                      kTextureUsageSampled | kTextureUsageTransferDst);
  if (!fields_[0] || !fields_[1] || !dummy_ocean_ || !linear_clamp_ || !linear_wrap_) {
    RX_WARN("shoreline wetting allocation failed; feature disabled");
    Destroy(device);
    return false;
  }

  // Clear the ping-pong fields to dry and park them in GENERAL (they stay
  // there, storage-written each frame and sampled by the scene pass). The dummy
  // ocean is left shader-readable for the Gerstner path.
  device.ImmediateSubmit([&](CommandList& cmd) {
    const f32 zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (GpuImage& field : fields_) {
      cmd.Barrier(Transition(field, ResourceState::kUndefined, ResourceState::kCopyDst));
      cmd.ClearColor(field, zero);
      cmd.Barrier(Transition(field, ResourceState::kCopyDst, ResourceState::kGeneral));
    }
    cmd.Barrier(Transition(dummy_ocean_, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.ClearColor(dummy_ocean_, zero);
    cmd.Barrier(Transition(dummy_ocean_, ResourceState::kCopyDst, ResourceState::kShaderReadAll));
  });
  return true;
}

void ShoreWetting::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (GpuImage& field : fields_) device.DestroyImage(field);
  device.DestroyImage(dummy_ocean_);
  have_prev_ = false;
}

void ShoreWetting::BeginFrame(const Vec3& camera_eye) {
  // Advance the ping-pong: read what was written last frame, write the other.
  read_index_ = write_index_;
  write_index_ ^= 1u;

  prev_origin_[0] = origin_[0];
  prev_origin_[1] = origin_[1];
  // Snap the origin to the texel grid so the field does not shimmer as the
  // camera slides; the min corner sits half an extent behind the eye.
  const f32 texel = kExtent / static_cast<f32>(kResolution);
  origin_[0] = std::floor((camera_eye.x - kExtent * 0.5f) / texel) * texel;
  origin_[1] = std::floor((camera_eye.z - kExtent * 0.5f) / texel) * texel;
  if (!have_prev_) {
    prev_origin_[0] = origin_[0];
    prev_origin_[1] = origin_[1];
    have_prev_ = true;
  }
}

void ShoreWetting::FieldParams(f32 out[4]) const {
  out[0] = origin_[0];
  out[1] = origin_[1];
  out[2] = 1.0f / kExtent;
  out[3] = 0.0f;
}

void ShoreWetting::AddToGraph(RenderGraph& graph, const Params& params) {
  if (!available()) return;
  graph.AddPass(
      "shore_wetting", [](RenderGraph::PassBuilder&) {},
      [this, params](PassContext& ctx) {
        ShorePush push{};
        push.field[0] = origin_[0];
        push.field[1] = origin_[1];
        push.field[2] = kExtent;
        push.field[3] = 1.0f / kExtent;
        push.prev_field[0] = prev_origin_[0];
        push.prev_field[1] = prev_origin_[1];
        for (u32 i = 0; i < 4; ++i) push.island[i] = params.island[i];
        push.params[0] = params.time;
        push.params[1] = params.dt;
        push.params[2] = params.drying_time;
        push.params[3] = params.fft_active ? 1.0f : 0.0f;

        // The ocean displacement is compute-written earlier this frame; make it
        // visible to this compute read before sampling it.
        if (params.fft_active && params.ocean_displacement) {
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        }

        BindingItem ocean =
            params.fft_active && params.ocean_displacement
                ? InGeneral(Bind::Combined(2, params.ocean_displacement, linear_wrap_))
                : Bind::Combined(2, dummy_ocean_.view, linear_wrap_);
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, fields_[write_index_]),
                InGeneral(Bind::Combined(1, fields_[read_index_].view, linear_clamp_)), ocean});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch(kResolution / 8, kResolution / 8, 1);
        // Sampled by the opaque scene pass (mesh.ps, env slot 30).
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });
}

}  // namespace rx::render
