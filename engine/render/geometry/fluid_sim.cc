#include "render/geometry/fluid_sim.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "core/log.h"
#include "shaders/fluid_sim_cs_hlsl.h"

namespace rx::render {
namespace {

// Solver push constants; mirrors PushData in fluid_sim.cs.hlsl.
struct FluidPush {
  f32 domain[4];   // origin.x, origin.z, extent, cell size l
  u32 control[4];  // resolution, phase, source_count, pad
  f32 sim[4];      // dt_sub, gravity, k_drag_water, ambient
  f32 lava0[4];    // T_liq, T_sol, eta0, k_eta
  f32 lava1[4];    // yield0, k_cool, r_sol, cold_drag_scale
};
static_assert(sizeof(FluidPush) == 80);

// GPU source record; mirrors the shader's Source struct (two float4s).
struct GpuSource {
  f32 pos_radius[4];  // world x, world z, unused, radius
  f32 params[4];      // rate, fluid, temperature, unused
};
static_assert(sizeof(GpuSource) == 32);

// Solver phases (control.y). Lava solves first each substep so water sees the
// crust + lava depth as bed; the four dispatches share one pipeline.
constexpr u32 kPhaseLavaFlux = 0u;
constexpr u32 kPhaseLavaIntegrate = 1u;
constexpr u32 kPhaseWaterFlux = 2u;
constexpr u32 kPhaseWaterIntegrate = 3u;

// Tunables the demo can lift into settings later; physical defaults from the
// brief (MAGFLOW / Chentanez). Water drag is a mild implicit damping.
constexpr f32 kGravity = 9.81f;
constexpr f32 kWaterDrag = 0.05f;
constexpr f32 kLavaTLiq = 1150.0f;      // liquidus (deg C)
constexpr f32 kLavaTSol = 800.0f;       // solidus (deg C)
constexpr f32 kLavaEta0 = 0.02f;        // Arrhenius mobility prefactor
constexpr f32 kLavaKEta = 0.02f;        // mobility temperature slope
constexpr f32 kLavaYield0 = 0.15f;      // yield head at the solidus (m)
constexpr f32 kLavaCooling = 0.02f;     // relaxation toward ambient
constexpr f32 kLavaSolidRate = 0.05f;   // crust growth rate (m/s)
constexpr f32 kLavaColdDrag = 4.0f;     // extra drag scale as lava cools

}  // namespace

bool FluidSim::Initialize(Device& device) {
  // Single compute pipeline; the phase (lava/water flux/integrate) is selected
  // by a push constant. Five storage images (bed, state in/out, active flux,
  // velocity) plus the bounded source buffer. State/velocity/bed are read as
  // storage loads here; the surface renderer samples them downstream.
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_fluid_sim_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kStorageImage},
                          {4, BindingType::kStorageImage},
                          {5, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(FluidPush),
      .debug_name = "fluid_sim",
  });
  if (!pipeline_) return false;

  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    params_[f] = device.CreateBuffer(sizeof(GpuParams), kBufferUsageUniform, true);
    sources_[f] = device.CreateBuffer(kMaxSources * sizeof(GpuSource), kBufferUsageStorage, true);
    if (!params_[f].mapped || !sources_[f].mapped) {
      RX_WARN("fluid sim buffer mapping failed; feature disabled");
      Destroy(device);
      return false;
    }
  }
  device_ = &device;
  return true;
}

void FluidSim::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  DestroyImages(device);
  for (u32 f = 0; f < Device::kMaxFramesInFlight; ++f) {
    device.DestroyBuffer(params_[f]);
    device.DestroyBuffer(sources_[f]);
  }
  configured_ = false;
  have_domain_ = false;
  disabled_ = false;
  accum_ = 0;
  bed_version_ = ~0ull;
  device_ = nullptr;
}

void FluidSim::DestroyImages(Device& device) {
  // Deferred: a reconfigure of a live domain runs while the previous frame's
  // transparent pass may still sample these images; immediate destruction
  // would pull them out from under in-flight work.
  if (bed_) device.DestroyImageDeferred(bed_);
  bed_ = {};
  for (GpuImage& s : state_) {
    if (s) device.DestroyImageDeferred(s);
    s = {};
  }
  if (flux_water_) device.DestroyImageDeferred(flux_water_);
  flux_water_ = {};
  if (flux_lava_) device.DestroyImageDeferred(flux_lava_);
  flux_lava_ = {};
  if (velocity_) device.DestroyImageDeferred(velocity_);
  velocity_ = {};
}

void FluidSim::Configure(Device& device, const FluidDomainDesc& desc) {
  DestroyImages(device);

  const Extent2D ext{desc.resolution, desc.resolution};
  // All fields live in GENERAL (storage read/write) and are sampled by the
  // surface renderer downstream. TransferDst: bed (CPU upload), state (initial
  // water), flux/velocity (cleared to zero). TransferSrc: state + velocity, so
  // the solver output is host-readable (offscreen test, GPU debugging).
  const TextureUsageFlags kBase = kTextureUsageStorage | kTextureUsageSampled;
  bed_ = device.CreateImage2D(Format::kR32Float, ext, kBase | kTextureUsageTransferDst);
  for (GpuImage& s : state_)
    s = device.CreateImage2D(Format::kRGBA32Float, ext,
                             kBase | kTextureUsageTransferDst | kTextureUsageTransferSrc);
  flux_water_ = device.CreateImage2D(Format::kRGBA32Float, ext, kBase | kTextureUsageTransferDst);
  flux_lava_ = device.CreateImage2D(Format::kRGBA32Float, ext, kBase | kTextureUsageTransferDst);
  velocity_ = device.CreateImage2D(Format::kRGBA16Float, ext,
                                   kBase | kTextureUsageTransferDst | kTextureUsageTransferSrc);
  if (!bed_ || !state_[0] || !state_[1] || !flux_water_ || !flux_lava_ || !velocity_) {
    RX_WARN("fluid sim image allocation failed; feature disabled until Destroy");
    DestroyImages(device);
    configured_ = false;
    disabled_ = true;  // latch: do not retry the allocation (and warn) every frame
    return;
  }

  const u32 cells = desc.resolution * desc.resolution;
  // Initial state: r=water depth, g=lava depth 0, b=T ambient, a=crust 0.
  std::vector<f32> init(static_cast<size_t>(cells) * 4, 0.0f);
  for (u32 i = 0; i < cells; ++i) {
    init[i * 4 + 0] = desc.initial_water ? desc.initial_water[i] : 0.0f;
    init[i * 4 + 2] = desc.ambient_temperature;
  }
  GpuBuffer bed_stage =
      device.CreateBuffer(static_cast<u64>(cells) * sizeof(f32), kBufferUsageTransferSrc, true);
  GpuBuffer state_stage =
      device.CreateBuffer(init.size() * sizeof(f32), kBufferUsageTransferSrc, true);
  if (!bed_stage.mapped || !state_stage.mapped) {
    // Recording a copy from an invalid/unmapped staging buffer is a backend
    // crash; treat it like the image-allocation failure above.
    RX_WARN("fluid sim staging allocation failed; feature disabled until Destroy");
    device.DestroyBuffer(bed_stage);
    device.DestroyBuffer(state_stage);
    DestroyImages(device);
    configured_ = false;
    disabled_ = true;
    return;
  }
  std::memcpy(bed_stage.mapped, desc.bed, static_cast<size_t>(cells) * sizeof(f32));
  std::memcpy(state_stage.mapped, init.data(), init.size() * sizeof(f32));

  device.ImmediateSubmit([&](CommandList& cmd) {
    const f32 zero[4] = {0, 0, 0, 0};
    BufferTextureCopy copy;

    cmd.Barrier(Transition(bed_, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.CopyBufferToTexture(bed_stage, bed_, {&copy, 1});
    cmd.Barrier(Transition(bed_, ResourceState::kCopyDst, ResourceState::kGeneral));

    // Seed both ping-pong slots (each substep writes B then A, but a clean
    // start keeps the very first sampled reads well defined).
    for (GpuImage& s : state_) {
      cmd.Barrier(Transition(s, ResourceState::kUndefined, ResourceState::kCopyDst));
      cmd.CopyBufferToTexture(state_stage, s, {&copy, 1});
      cmd.Barrier(Transition(s, ResourceState::kCopyDst, ResourceState::kGeneral));
    }
    for (GpuImage* img : {&flux_water_, &flux_lava_, &velocity_}) {
      cmd.Barrier(Transition(*img, ResourceState::kUndefined, ResourceState::kCopyDst));
      cmd.ClearColor(*img, zero);
      cmd.Barrier(Transition(*img, ResourceState::kCopyDst, ResourceState::kGeneral));
    }
  });
  device.DestroyBuffer(bed_stage);
  device.DestroyBuffer(state_stage);

  domain_ = desc;
  domain_.bed = nullptr;  // pointer lifetime is caller-owned; do not retain
  domain_.initial_water = nullptr;
  bed_version_ = desc.bed_version;
  read_ = 0;
  accum_ = 0;
  configured_ = true;
}

void FluidSim::UploadBed(Device& device, const FluidDomainDesc& desc) {
  const u32 cells = desc.resolution * desc.resolution;
  GpuBuffer stage =
      device.CreateBuffer(static_cast<u64>(cells) * sizeof(f32), kBufferUsageTransferSrc, true);
  if (!stage.mapped) {
    RX_WARN("fluid sim bed staging failed; keeping the previous bed");
    device.DestroyBuffer(stage);
    bed_version_ = desc.bed_version;  // consume the bump; do not retry-spam
    return;
  }
  std::memcpy(stage.mapped, desc.bed, static_cast<size_t>(cells) * sizeof(f32));
  device.ImmediateSubmit([&](CommandList& cmd) {
    BufferTextureCopy copy;
    // kUndefined-as-source orders behind ALL prior work (the engine's
    // convention for full-image rewrites): the previous frame's vertex and
    // fragment stages sample bed_, and a kGeneral-source transition would only
    // wait on compute. Contents are discarded, but the copy rewrites the whole
    // image.
    cmd.Barrier(Transition(bed_, ResourceState::kUndefined, ResourceState::kCopyDst));
    cmd.CopyBufferToTexture(stage, bed_, {&copy, 1});
    cmd.Barrier(Transition(bed_, ResourceState::kCopyDst, ResourceState::kGeneral));
  });
  device.DestroyBuffer(stage);
  bed_version_ = desc.bed_version;
}

void FluidSim::AddToGraph(RenderGraph& graph, const UpdateParams& params) {
  have_domain_ = false;
  if (!available() || disabled_ || !params.domain || !device_) return;
  const FluidDomainDesc& desc = *params.domain;
  if (!desc.bed || desc.resolution == 0 || desc.resolution > kMaxResolution) return;

  // (Re)configure on a domain change (resolution/extent/origin); re-upload the
  // bed alone on a bed_version bump (an obstacle stamped in/out).
  const bool reconfigure =
      !configured_ || desc.resolution != domain_.resolution || desc.extent != domain_.extent ||
      desc.origin[0] != domain_.origin[0] || desc.origin[1] != domain_.origin[1];
  if (reconfigure) {
    Configure(*device_, desc);
    if (!configured_) return;
  } else if (desc.bed_version != bed_version_) {
    UploadBed(*device_, desc);
  }
  have_domain_ = true;

  const u32 slot = params.frame_slot % Device::kMaxFramesInFlight;
  const f32 l = desc.extent / static_cast<f32>(desc.resolution);

  // Params CB for the surface renderer.
  GpuParams gp{};
  gp.origin[0] = desc.origin[0];
  gp.origin[1] = desc.origin[1];
  gp.extent = desc.extent;
  gp.texel = l;
  gp.resolution = static_cast<f32>(desc.resolution);
  std::memcpy(params_[slot].mapped, &gp, sizeof(gp));

  // Bounded per-frame sources, packed for the shader. A null pointer means no
  // sources regardless of the count — the shader must never consume the slot's
  // stale records from a previous frame.
  u32 source_count = params.sources ? std::min(params.source_count, kMaxSources) : 0u;
  if (source_count > 0) {
    GpuSource* dst = static_cast<GpuSource*>(sources_[slot].mapped);
    for (u32 i = 0; i < source_count; ++i) {
      const FluidSource& s = params.sources[i];
      dst[i].pos_radius[0] = s.position.x;
      dst[i].pos_radius[1] = s.position.z;
      dst[i].pos_radius[2] = 0.0f;
      dst[i].pos_radius[3] = s.radius;
      dst[i].params[0] = s.rate;
      dst[i].params[1] = static_cast<f32>(s.fluid);
      dst[i].params[2] = s.temperature;
      dst[i].params[3] = 0.0f;
    }
  }

  // Fixed substeps with a per-frame cap; the remainder carries across frames so
  // the sim is frame-rate independent and deterministic.
  accum_ += std::max(params.dt, 0.0f);
  u32 substeps = static_cast<u32>(accum_ / kSubstepDt);
  substeps = std::min(substeps, kMaxSubsteps);
  accum_ -= static_cast<f32>(substeps) * kSubstepDt;
  // A dt larger than the per-frame substep budget must not accumulate as time
  // debt (it would spiral: every later frame runs the cap and never catches
  // up). Drop the excess — the sim slows down instead of death-spiralling.
  accum_ = std::min(accum_, kSubstepDt);
  if (substeps == 0) return;  // read side unchanged; renderer still has state

  FluidPush push{};
  push.domain[0] = desc.origin[0];
  push.domain[1] = desc.origin[1];
  push.domain[2] = desc.extent;
  push.domain[3] = l;
  push.control[0] = desc.resolution;
  push.control[2] = source_count;
  push.sim[0] = kSubstepDt;
  push.sim[1] = kGravity;
  push.sim[2] = kWaterDrag;
  push.sim[3] = desc.ambient_temperature;
  push.lava0[0] = kLavaTLiq;
  push.lava0[1] = kLavaTSol;
  push.lava0[2] = kLavaEta0;
  push.lava0[3] = kLavaKEta;
  push.lava1[0] = kLavaYield0;
  push.lava1[1] = kLavaCooling;
  push.lava1[2] = kLavaSolidRate;
  push.lava1[3] = kLavaColdDrag;

  const u32 groups = (desc.resolution + 7u) / 8u;
  const u32 read = read_;  // authoritative slot A; lava writes B, water writes A

  graph.AddPass(
      "fluid_sim", [](RenderGraph::PassBuilder&) {},
      [this, push, groups, read, slot, substeps](PassContext& ctx) mutable {
        ctx.cmd->BindPipeline(pipeline_);
        // Order this frame's solver behind the PREVIOUS frame's use of these
        // images: its compute wrote state/flux (read-after-write) and its
        // vertex/fragment stages sampled state/bed/velocity (write-after-read).
        // The frame fence only covers frame N-2 and this pass declares no graph
        // resources, so without this barrier adjacent frames' GPU work overlaps.
        ctx.cmd->MemoryBarrier(BarrierScope::kAllCommands, BarrierScope::kComputeReadWrite);
        const u32 a = read;        // input / final output slot
        const u32 b = read ^ 1u;   // lava scratch slot

        for (u32 step = 0; step < substeps; ++step) {
          // Phase 0: lava flux. Reads state[A], writes flux_lava in place.
          push.control[1] = kPhaseLavaFlux;
          ctx.cmd->BindTransient(
              0, {InGeneral(Bind::Storage(0, state_[a])), InGeneral(Bind::Storage(1, state_[b])),
                  InGeneral(Bind::Storage(2, bed_)), InGeneral(Bind::Storage(3, flux_lava_)),
                  InGeneral(Bind::Storage(4, velocity_)), Bind::StorageBuffer(5, sources_[slot])});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch(groups, groups, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

          // Phase 1: lava integrate (+thermal +solidify). state[A] -> state[B].
          push.control[1] = kPhaseLavaIntegrate;
          ctx.cmd->BindTransient(
              0, {InGeneral(Bind::Storage(0, state_[a])), InGeneral(Bind::Storage(1, state_[b])),
                  InGeneral(Bind::Storage(2, bed_)), InGeneral(Bind::Storage(3, flux_lava_)),
                  InGeneral(Bind::Storage(4, velocity_)), Bind::StorageBuffer(5, sources_[slot])});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch(groups, groups, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

          // Phase 2: water flux. Reads state[B] (sees lava as bed), writes
          // flux_water in place.
          push.control[1] = kPhaseWaterFlux;
          ctx.cmd->BindTransient(
              0, {InGeneral(Bind::Storage(0, state_[b])), InGeneral(Bind::Storage(1, state_[a])),
                  InGeneral(Bind::Storage(2, bed_)), InGeneral(Bind::Storage(3, flux_water_)),
                  InGeneral(Bind::Storage(4, velocity_)), Bind::StorageBuffer(5, sources_[slot])});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch(groups, groups, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

          // Phase 3: water integrate (+quench). state[B] -> state[A]; owns the
          // full RGBA write, so quench editing g/a is race-free.
          push.control[1] = kPhaseWaterIntegrate;
          ctx.cmd->BindTransient(
              0, {InGeneral(Bind::Storage(0, state_[b])), InGeneral(Bind::Storage(1, state_[a])),
                  InGeneral(Bind::Storage(2, bed_)), InGeneral(Bind::Storage(3, flux_water_)),
                  InGeneral(Bind::Storage(4, velocity_)), Bind::StorageBuffer(5, sources_[slot])});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch(groups, groups, 1);
          // Between substeps another compute read follows; after the last the
          // surface renderer samples state/velocity in the graphics stages.
          if (step + 1 < substeps)
            ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          else
            ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        }
      });
  // Each substep returns the authoritative state to slot A, so the read side is
  // stable across frames; read_ never flips.
}

// --- test seam (readback of the solver output) ---------------------------
const GpuImage& FluidSimProbe::state(const FluidSim& sim) { return sim.state_[sim.read_]; }
const GpuImage& FluidSimProbe::velocity(const FluidSim& sim) { return sim.velocity_; }

}  // namespace rx::render
