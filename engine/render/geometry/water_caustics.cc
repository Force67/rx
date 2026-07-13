#include "render/geometry/water_caustics.h"

#include "core/log.h"
#include "shaders/water_caustics_cs_hlsl.h"

namespace rx::render {
namespace {

struct CausticPush {
  f32 sun[4];      // xyz sun travel direction (normalized), w time (s)
  f32 params[4];   // x tile (m), y rest height (m), z receiver depth (m), w eta
  f32 misc[4];     // x fixed-point scale, y fft flag, zw unused
  u32 control[4];  // x phase, yzw unused
};
static_assert(sizeof(CausticPush) == 64);

constexpr f32 kEta = 1.0f / 1.33f;       // air -> water refraction ratio
constexpr f32 kFixedPointScale = 4096.f;  // photon energy fixed-point quantum

}  // namespace

bool WaterCaustics::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_water_caustics_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(CausticPush),
      .debug_name = "water_caustics",
  });
  if (!pipeline_) return false;

  linear_wrap_ = device.GetSampler({.min_filter = Filter::kLinear,
                                    .mag_filter = Filter::kLinear,
                                    .address_u = AddressMode::kRepeat,
                                    .address_v = AddressMode::kRepeat});

  caustic_ = device.CreateImage2D(Format::kRG16Float, {kSize, kSize},
                                  kTextureUsageSampled | kTextureUsageStorage);
  accum_ = device.CreateBuffer(static_cast<u64>(kSize) * kSize * sizeof(u32),
                               kBufferUsageStorage, false);
  dummy_ocean_ = device.CreateImage2D(Format::kRGBA16Float, {1, 1},
                                      kTextureUsageSampled | kTextureUsageTransferDst);
  if (!caustic_ || !accum_ || !dummy_ocean_ || !linear_wrap_) {
    RX_WARN("water caustics allocation failed; feature disabled");
    Destroy(device);
    return false;
  }

  // Park the caustic map in GENERAL (storage-written each frame and sampled by
  // the scene pass) and leave the dummy ocean shader-readable for the Gerstner
  // path, mirroring the shoreline-wetting setup.
  device.ImmediateSubmit([&](CommandList& cmd) {
    const f32 zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    cmd.Barrier(Transition(caustic_, ResourceState::kUndefined, ResourceState::kGeneral));
    cmd.Barrier(Transition(dummy_ocean_, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.ClearColor(dummy_ocean_, zero);
    cmd.Barrier(Transition(dummy_ocean_, ResourceState::kCopyDst, ResourceState::kShaderReadAll));
  });
  return true;
}

void WaterCaustics::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  if (caustic_) device.DestroyImage(caustic_);
  caustic_ = {};
  device.DestroyBuffer(accum_);
  if (dummy_ocean_) device.DestroyImage(dummy_ocean_);
  dummy_ocean_ = {};
}

void WaterCaustics::AddToGraph(RenderGraph& graph, const Params& params) {
  if (!available()) return;
  graph.AddPass(
      "water_caustics", [](RenderGraph::PassBuilder&) {},
      [this, params](PassContext& ctx) {
        const bool fft = params.fft_active && params.ocean_displacement && params.ocean_normal;

        CausticPush push{};
        push.sun[0] = params.sun_travel.x;
        push.sun[1] = params.sun_travel.y;
        push.sun[2] = params.sun_travel.z;
        push.sun[3] = params.time;
        push.params[0] = kTile;
        push.params[1] = params.rest_height;
        push.params[2] = params.receiver_depth;
        push.params[3] = kEta;
        push.misc[0] = kFixedPointScale;
        push.misc[1] = fft ? 1.0f : 0.0f;

        // The FFT ocean writes its maps earlier this frame; make those writes
        // visible to our sampled reads.
        if (fft) ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        TextureView disp = fft ? params.ocean_displacement : dummy_ocean_.view;
        TextureView norm = fft ? params.ocean_normal : dummy_ocean_.view;
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, accum_), Bind::Storage(1, caustic_),
                fft ? InGeneral(Bind::Combined(2, disp, linear_wrap_))
                    : Bind::Combined(2, disp, linear_wrap_),
                fft ? InGeneral(Bind::Combined(3, norm, linear_wrap_))
                    : Bind::Combined(3, norm, linear_wrap_)});

        const u32 groups = kSize / 8;
        // Phase 0: clear the accumulation buffer.
        push.control[0] = 0u;
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch(groups, groups, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Phase 1: scatter one photon per surface texel (atomic splat).
        push.control[0] = 1u;
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch(groups, groups, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Phase 2: normalize + write the RG16F map (+ wave shadow).
        push.control[0] = 2u;
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch(groups, groups, 1);
        // Sampled by the opaque scene pass (mesh.ps/mesh_rt.ps, env slot 34).
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });
}

}  // namespace rx::render
