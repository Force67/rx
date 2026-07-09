#include "render/geometry/ocean_fft.h"

#include <cmath>
#include <random>
#include <vector>

#include "core/log.h"
#include "shaders/ocean_fft_cs_hlsl.h"
#include "shaders/ocean_finalize_cs_hlsl.h"
#include "shaders/ocean_normals_cs_hlsl.h"
#include "shaders/ocean_spectrum_cs_hlsl.h"

namespace rx::render {
namespace {

struct SpectrumPush {
  u32 size;
  f32 patch_size;
  f32 time;
  f32 pad0;
};
struct FftPush {
  u32 size;
  u32 log_size;
  u32 horizontal;
  u32 pad0;
};
struct FinalizePush {
  u32 size;
  f32 choppiness;
  f32 amplitude;
  f32 pad0;
};
struct NormalsPush {
  u32 size;
  f32 texel_world;
  f32 foam_scale;
  f32 pad0;
};

// Phillips spectrum with directional spread toward the wind and a small-wave
// cutoff so the highest frequencies don't alias against the grid.
f32 Phillips(f32 kx, f32 kz, f32 wind_speed, f32 wind_x, f32 wind_z) {
  f32 k2 = kx * kx + kz * kz;
  if (k2 < 1e-8f) return 0.0f;
  const f32 g = 9.81f;
  f32 l = wind_speed * wind_speed / g;  // largest wave from this wind
  f32 k = std::sqrt(k2);
  f32 kdw = (kx * wind_x + kz * wind_z) / k;
  constexpr f32 kAmplitude = 0.6f;
  f32 p = kAmplitude * std::exp(-1.0f / (k2 * l * l)) / (k2 * k2) * (kdw * kdw);
  if (kdw < 0.0f) p *= 0.25f;               // damp waves running against the wind
  const f32 small_cut = 0.35f;               // meters
  p *= std::exp(-k2 * small_cut * small_cut);
  return p;
}

}  // namespace

bool OceanFft::Initialize(Device& device) {
  spectrum_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_spectrum_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kSampledImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(SpectrumPush),
      .debug_name = "ocean_spectrum",
  });
  fft_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_fft_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(FftPush),
      .debug_name = "ocean_fft",
  });
  finalize_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_finalize_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(FinalizePush),
      .debug_name = "ocean_finalize",
  });
  normals_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_normals_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(NormalsPush),
      .debug_name = "ocean_normals",
  });
  if (!spectrum_pipeline_ || !fft_pipeline_ || !finalize_pipeline_ || !normals_pipeline_) {
    RX_ERROR("ocean fft pipeline creation failed");
    return false;
  }

  h0_ = device.CreateImage2D(Format::kRGBA32Float, {kSize, kSize},
                             kTextureUsageSampled | kTextureUsageTransferDst);
  for (GpuImage* img : {&spectrum_[0], &spectrum_z_[0]}) {
    *img = device.CreateImage2D(Format::kRGBA32Float, {kSize, kSize}, kTextureUsageStorage);
  }
  displacement_ = device.CreateImage2D(Format::kRGBA16Float, {kSize, kSize},
                                       kTextureUsageStorage | kTextureUsageSampled);
  normal_foam_ = device.CreateImage2D(Format::kRGBA16Float, {kSize, kSize},
                                      kTextureUsageStorage | kTextureUsageSampled);
  if (!h0_ || !spectrum_[0] || !spectrum_z_[0] || !displacement_ || !normal_foam_) {
    RX_WARN("ocean fft allocation failed");
    Destroy(device);
    return false;
  }

  // h0(k) on the CPU with a FIXED seed: the whole animation is then a pure
  // function of time, which the golden-image harness depends on.
  std::mt19937 rng(1337);
  std::normal_distribution<f32> gauss(0.0f, 1.0f);
  const f32 wind_speed = 7.5f;
  const f32 inv_sqrt2 = 0.70710678f;
  f32 wind_x = 0.8f, wind_z = 0.6f;
  std::vector<f32> h0(static_cast<size_t>(kSize) * kSize * 4);
  for (u32 y = 0; y < kSize; ++y) {
    for (u32 x = 0; x < kSize; ++x) {
      f32 n = static_cast<f32>(x) - kSize * 0.5f;
      f32 m = static_cast<f32>(y) - kSize * 0.5f;
      f32 kx = 2.0f * 3.14159265f * n / kPatchSize;
      f32 kz = 2.0f * 3.14159265f * m / kPatchSize;
      f32 ph = std::sqrt(Phillips(kx, kz, wind_speed, wind_x, wind_z));
      f32 phm = std::sqrt(Phillips(-kx, -kz, wind_speed, wind_x, wind_z));
      size_t o = (static_cast<size_t>(y) * kSize + x) * 4;
      h0[o + 0] = gauss(rng) * inv_sqrt2 * ph;
      h0[o + 1] = gauss(rng) * inv_sqrt2 * ph;
      h0[o + 2] = gauss(rng) * inv_sqrt2 * phm;
      h0[o + 3] = gauss(rng) * inv_sqrt2 * phm;
    }
  }
  GpuBuffer staging = device.CreateBuffer(h0.size() * sizeof(f32), kBufferUsageTransferSrc, true);
  if (!staging.mapped) return false;
  std::memcpy(staging.mapped, h0.data(), h0.size() * sizeof(f32));
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(h0_, ResourceState::kUndefined, ResourceState::kCopyDst));
    BufferTextureCopy copy;
    cmd.CopyBufferToTexture(staging, h0_, {&copy, 1});
    cmd.Barrier(Transition(h0_, ResourceState::kCopyDst, ResourceState::kShaderReadCompute));
    TextureBarrier to_general[4] = {
        Transition(spectrum_[0], ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(spectrum_z_[0], ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(displacement_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(normal_foam_, ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  device.DestroyBuffer(staging);
  return true;
}

void OceanFft::Destroy(Device& device) {
  for (PipelineHandle* p :
       {&spectrum_pipeline_, &fft_pipeline_, &finalize_pipeline_, &normals_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage* img : {&h0_, &spectrum_[0], &spectrum_[1], &spectrum_z_[0], &spectrum_z_[1],
                        &displacement_, &normal_foam_}) {
    if (*img) device.DestroyImage(*img);
    *img = {};
  }
}

void OceanFft::AddToGraph(RenderGraph& graph, f32 time) {
  if (!available()) return;
  graph.AddPass(
      "ocean_fft", [](RenderGraph::PassBuilder&) {},
      [this, time](PassContext& ctx) {
        SpectrumPush spec{kSize, kPatchSize, time, 0.0f};
        ctx.cmd->BindPipeline(spectrum_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Sampled(0, h0_),
                                   Bind::Storage(1, spectrum_[0]),
                                   Bind::Storage(2, spectrum_z_[0])});
        ctx.cmd->Push(spec);
        ctx.cmd->Dispatch(kSize / 8, kSize / 8, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        // Inverse FFT: rows then columns, for both spectra.
        ctx.cmd->BindPipeline(fft_pipeline_);
        for (u32 dir = 0; dir < 2; ++dir) {
          for (GpuImage* img : {&spectrum_[0], &spectrum_z_[0]}) {
            FftPush fft{kSize, 8, dir, 0};
            ctx.cmd->BindTransient(0, {Bind::Storage(0, *img)});
            ctx.cmd->Push(fft);
            ctx.cmd->Dispatch(kSize, 1, 1);
          }
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
        }

        FinalizePush fin{kSize, /*choppiness=*/1.1f, /*amplitude=*/1.0f, 0.0f};
        ctx.cmd->BindPipeline(finalize_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, spectrum_[0]),
                                   Bind::Storage(1, spectrum_z_[0]),
                                   Bind::Storage(2, displacement_)});
        ctx.cmd->Push(fin);
        ctx.cmd->Dispatch(kSize / 8, kSize / 8, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

        NormalsPush nrm{kSize, kPatchSize / kSize, /*foam_scale=*/2.2f, 0.0f};
        ctx.cmd->BindPipeline(normals_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, displacement_),
                                   Bind::Storage(1, normal_foam_)});
        ctx.cmd->Push(nrm);
        ctx.cmd->Dispatch(kSize / 8, kSize / 8, 1);
        // The maps are sampled by the water vertex/pixel shaders.
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
      });
}

}  // namespace rx::render
