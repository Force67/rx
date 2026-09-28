#include "rxe/render/geometry/ocean_fft.h"

#include <math.h>
#include <string.h>

#include "base/containers/vector.h"
#include "base/memory/mem_ops.h"
#include "foundation/build_config/types.h"
#include "foundation/logging/log.h"
#include "shaders/ocean_fft_cs_hlsl.h"
#include "shaders/ocean_finalize_cs_hlsl.h"
#include "shaders/ocean_normals_cs_hlsl.h"
#include "shaders/ocean_spectrum_cs_hlsl.h"

namespace rx::render {
namespace {

// The spectrum below was authored, and golden-imaged, against libstdc++'s
// std::mt19937 and std::normal_distribution<float>. These reproduce both bit
// for bit: the engine's algorithm and seeding are fixed by the standard, and
// the distribution follows libstdc++ (Marsaglia's polar method over
// generate_canonical<float, 24>, the second deviate cached), including where
// its arithmetic is float and where it is double.
class Mt19937 {
 public:
  explicit Mt19937(u32 seed) {
    state_[0] = seed;
    for (u32 i = 1; i < kN; ++i) {
      state_[i] = 1812433253u * (state_[i - 1] ^ (state_[i - 1] >> 30)) + i;
    }
  }

  u32 operator()() {
    if (index_ >= kN) Twist();
    u32 y = state_[index_++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
  }

 private:
  static constexpr u32 kN = 624;
  static constexpr u32 kM = 397;

  void Twist() {
    for (u32 i = 0; i < kN; ++i) {
      const u32 y = (state_[i] & 0x80000000u) | (state_[(i + 1) % kN] & 0x7fffffffu);
      state_[i] = state_[(i + kM) % kN] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
    }
    index_ = 0;
  }

  u32 state_[kN];
  u32 index_ = kN;
};

// std::normal_distribution<float> with mean 0 and stddev 1, as libstdc++.
class StandardNormal {
 public:
  f32 operator()(Mt19937& rng) {
    f32 result;
    if (saved_available_) {
      saved_available_ = false;
      result = saved_;
    } else {
      f32 x, y, r2;
      do {
        x = static_cast<f32>(static_cast<f64>(2.0f * Canonical(rng)) - 1.0);
        y = static_cast<f32>(static_cast<f64>(2.0f * Canonical(rng)) - 1.0);
        r2 = x * x + y * y;
      } while (r2 > 1.0 || r2 == 0.0);
      const f32 mult = ::sqrtf(-2 * ::logf(r2) / r2);
      saved_ = x * mult;
      saved_available_ = true;
      result = y * mult;
    }
    // The distribution's own scale and shift: 1 and 0, but +0 still turns a
    // -0 into +0.
    return result * 1.0f + 0.0f;
  }

 private:
  // generate_canonical<float, 24>: one 32-bit draw, clamped below 1.
  static f32 Canonical(Mt19937& rng) {
    const f32 value = static_cast<f32>(rng()) / 4294967296.0f;
    return value >= 1.0f ? ::nextafterf(1.0f, 0.0f) : value;
  }

  f32 saved_ = 0;
  bool saved_available_ = false;
};


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
  f32 k = ::sqrtf(k2);
  f32 kdw = (kx * wind_x + kz * wind_z) / k;
  constexpr f32 kAmplitude = 0.6f;
  f32 p = kAmplitude * ::expf(-1.0f / (k2 * l * l)) / (k2 * k2) * (kdw * kdw);
  if (kdw < 0.0f) p *= 0.25f;               // damp waves running against the wind
  const f32 small_cut = 0.35f;               // meters
  p *= ::expf(-k2 * small_cut * small_cut);
  return p;
}

}  // namespace

bool OceanFft::Initialize(gpu::Device& device) {
  spectrum_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_spectrum_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kSampledImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<SpectrumPush>(),
      .debug_name = "ocean_spectrum",
  });
  fft_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_fft_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<FftPush>(),
      .debug_name = "ocean_fft",
  });
  finalize_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_finalize_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage},
                          {2, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<FinalizePush>(),
      .debug_name = "ocean_finalize",
  });
  normals_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ocean_normals_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                          {1, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = gpu::PushSize<NormalsPush>(),
      .debug_name = "ocean_normals",
  });
  if (!spectrum_pipeline_ || !fft_pipeline_ || !finalize_pipeline_ || !normals_pipeline_) {
    RX_ERROR("ocean fft pipeline creation failed");
    return false;
  }

  h0_ = device.CreateImage2D(gpu::Format::kRGBA32Float, {kSize, kSize},
                             gpu::kTextureUsageSampled | gpu::kTextureUsageTransferDst);
  for (gpu::GpuImage* img : {&spectrum_[0], &spectrum_z_[0]}) {
    *img = device.CreateImage2D(gpu::Format::kRGBA32Float, {kSize, kSize}, gpu::kTextureUsageStorage);
  }
  displacement_ = device.CreateImage2D(gpu::Format::kRGBA16Float, {kSize, kSize},
                                       gpu::kTextureUsageStorage | gpu::kTextureUsageSampled);
  normal_foam_ = device.CreateImage2D(gpu::Format::kRGBA16Float, {kSize, kSize},
                                      gpu::kTextureUsageStorage | gpu::kTextureUsageSampled);
  if (!h0_ || !spectrum_[0] || !spectrum_z_[0] || !displacement_ || !normal_foam_) {
    RX_WARN("ocean fft allocation failed");
    Destroy(device);
    return false;
  }

  // h0(k) on the CPU with a FIXED seed: the whole animation is then a pure
  // function of time, which the golden-image harness depends on.
  Mt19937 rng(1337);
  StandardNormal gauss;
  const f32 wind_speed = 7.5f;
  const f32 inv_sqrt2 = 0.70710678f;
  f32 wind_x = 0.8f, wind_z = 0.6f;
  base::Vector<f32> h0(static_cast<size_t>(kSize) * kSize * 4);
  for (u32 y = 0; y < kSize; ++y) {
    for (u32 x = 0; x < kSize; ++x) {
      f32 n = static_cast<f32>(x) - kSize * 0.5f;
      f32 m = static_cast<f32>(y) - kSize * 0.5f;
      f32 kx = 2.0f * 3.14159265f * n / kPatchSize;
      f32 kz = 2.0f * 3.14159265f * m / kPatchSize;
      f32 ph = ::sqrtf(Phillips(kx, kz, wind_speed, wind_x, wind_z));
      f32 phm = ::sqrtf(Phillips(-kx, -kz, wind_speed, wind_x, wind_z));
      size_t o = (static_cast<size_t>(y) * kSize + x) * 4;
      h0[o + 0] = gauss(rng) * inv_sqrt2 * ph;
      h0[o + 1] = gauss(rng) * inv_sqrt2 * ph;
      h0[o + 2] = gauss(rng) * inv_sqrt2 * phm;
      h0[o + 3] = gauss(rng) * inv_sqrt2 * phm;
    }
  }
  gpu::GpuBuffer staging = device.CreateBuffer(h0.size() * sizeof(f32), gpu::kBufferUsageTransferSrc, true);
  if (!staging.mapped) return false;
  base::MemCopy(staging.mapped, h0.data(), h0.size() * sizeof(f32));
  device.ImmediateSubmit([&](gpu::CommandList& cmd) {
    cmd.Barrier(gpu::Transition(h0_, gpu::ResourceState::kUndefined, gpu::ResourceState::kCopyDst));
    gpu::BufferTextureCopy copy;
    cmd.CopyBufferToTexture(staging, h0_, base::Span(&copy, 1));
    cmd.Barrier(gpu::Transition(h0_, gpu::ResourceState::kCopyDst, gpu::ResourceState::kShaderReadCompute));
    gpu::TextureBarrier to_general[4] = {
        gpu::Transition(spectrum_[0], gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(spectrum_z_[0], gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(displacement_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral),
        gpu::Transition(normal_foam_, gpu::ResourceState::kUndefined, gpu::ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  device.DestroyBuffer(staging);
  return true;
}

void OceanFft::Destroy(gpu::Device& device) {
  for (gpu::PipelineHandle* p :
       {&spectrum_pipeline_, &fft_pipeline_, &finalize_pipeline_, &normals_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (gpu::GpuImage* img : {&h0_, &spectrum_[0], &spectrum_[1], &spectrum_z_[0], &spectrum_z_[1],
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
        ctx.cmd->BindTransient(0, {gpu::Bind::Sampled(0, h0_),
                                   gpu::Bind::Storage(1, spectrum_[0]),
                                   gpu::Bind::Storage(2, spectrum_z_[0])});
        ctx.cmd->Push(spec);
        ctx.cmd->Dispatch(kSize / 8, kSize / 8, 1);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);

        // Inverse FFT: rows then columns, for both spectra.
        ctx.cmd->BindPipeline(fft_pipeline_);
        for (u32 dir = 0; dir < 2; ++dir) {
          for (gpu::GpuImage* img : {&spectrum_[0], &spectrum_z_[0]}) {
            FftPush fft{kSize, 8, dir, 0};
            ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, *img)});
            ctx.cmd->Push(fft);
            ctx.cmd->Dispatch(kSize, 1, 1);
          }
          ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);
        }

        FinalizePush fin{kSize, /*choppiness=*/1.1f, /*amplitude=*/1.0f, 0.0f};
        ctx.cmd->BindPipeline(finalize_pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, spectrum_[0]),
                                   gpu::Bind::Storage(1, spectrum_z_[0]),
                                   gpu::Bind::Storage(2, displacement_)});
        ctx.cmd->Push(fin);
        ctx.cmd->Dispatch(kSize / 8, kSize / 8, 1);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);

        NormalsPush nrm{kSize, kPatchSize / kSize, /*foam_scale=*/2.2f, 0.0f};
        ctx.cmd->BindPipeline(normals_pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::Storage(0, displacement_),
                                   gpu::Bind::Storage(1, normal_foam_)});
        ctx.cmd->Push(nrm);
        ctx.cmd->Dispatch(kSize / 8, kSize / 8, 1);
        // The maps are sampled by the water vertex/pixel shaders.
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kGraphicsRead);
      });
}

}  // namespace rx::render
