#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "render/rhi/device.h"
#include "render/core/bindless.h"
#include "render/gi/raytracing.h"
#include "shaders/recon_restir_di_temporal_cs_hlsl.h"
#include "shaders/path_continue_cs_hlsl.h"
#include "shaders/recon_restir_di_spatial_cs_hlsl.h"
#include "shaders/recon_restir_temporal_cs_hlsl.h"
#include "shaders/recon_restir_spatial_cs_hlsl.h"

using namespace rx;
using namespace rx::render;

namespace {

constexpr u32 kWidth = 128, kHeight = 128, kPixels = kWidth * kHeight;
constexpr u32 kSkyW = 128, kSkyH = 64;
constexpr f32 kPi = 3.14159265359f;

struct Push {
  f32 sun_direction[4] = {0, -1, 0, 1};
  f32 sun_color[4] = {1, 1, 1, 0};
  u32 size[2] = {kWidth, kHeight};
  u32 frame_index = 0;
  u32 light_count = 1;
  u32 candidates = 8;
  u32 sky_candidates = 0;
  f32 m_max = 180;
  f32 reset = 1;
  f32 m_max_sky = 80;
  f32 pad[3] = {};
};

}  // namespace

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0
                     ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = true;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    std::printf("path_sampling_test: SKIP, GPU unavailable\n");
    return 0;
  }
  int failures = 0;
  auto check = [&](bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
  };
  ComputePipelineDesc pd;
  pd.shader = RX_SHADER(k_recon_restir_di_temporal_cs_hlsl);
  pd.sets.resize(1);
  for (u32 i = 0; i < 19; ++i) {
    BindingType type = BindingType::kSampledImage;
    if (i < 2 || i == 15 || i == 16) type = BindingType::kStorageImage;
    if (i == 12 || i == 14) type = BindingType::kStorageBuffer;
    if (i == 13) type = BindingType::kCombinedTextureSampler;
    pd.sets[0].slots.push_back({i, type});
  }
  pd.push_constant_size = PushSize<Push>();
  PipelineHandle pipeline = device->CreateComputePipeline(pd);
  if (!pipeline) return 1;

  std::vector<GpuImage> owned;
  auto input = [&](Format format, const void* pixel, u32 pixel_bytes) {
    std::vector<u8> data(kPixels * pixel_bytes);
    for (u32 i = 0; i < kPixels; ++i)
      std::memcpy(data.data() + i * pixel_bytes, pixel, pixel_bytes);
    GpuImage image = device->CreateImage2D(
        format, {kWidth, kHeight}, kTextureUsageSampled | kTextureUsageTransferDst);
    GpuBuffer staging = device->CreateBufferWithData(
        {data.data(), data.size()}, kBufferUsageTransferSrc);
    if (!image || !staging) std::exit(1);
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kCopyDst));
      BufferTextureCopy copy{.extent = {kWidth, kHeight}};
      cmd.CopyBufferToTexture(staging, image, {&copy, 1});
      cmd.Barrier(Transition(image, ResourceState::kCopyDst, ResourceState::kShaderReadCompute));
    });
    device->DestroyBuffer(staging);
    owned.push_back(image);
    return image;
  };
  const f32 pos[4] = {0, 0, 0, 1}, normal[4] = {.5f, 1, .5f, 1};
  const f32 zero[4] = {}, depth = 1;
  const u32 material = 0;
  GpuImage position = input(Format::kRGBA32Float, pos, sizeof(pos));
  GpuImage nr = input(Format::kRGBA32Float, normal, sizeof(normal));
  GpuImage vz = input(Format::kR32Float, &depth, sizeof(depth));
  GpuImage id = input(Format::kR32Uint, &material, sizeof(material));
  GpuImage motion = input(Format::kRG32Float, zero, sizeof(f32) * 2);
  GpuImage empty = input(Format::kRGBA32Float, zero, sizeof(zero));
  GpuImage sky = device->CreateImageCube(Format::kRGBA32Float, 1, kTextureUsageSampled);
  GpuImage out[4];
  for (GpuImage& image : out) {
    image = device->CreateImage2D(Format::kRGBA32Float, {kWidth, kHeight},
                                 kTextureUsageStorage | kTextureUsageTransferSrc);
    if (!image) return 1;
  }
  if (!sky) return 1;
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(sky, ResourceState::kUndefined, ResourceState::kShaderReadCompute));
    for (GpuImage& image : out)
      cmd.Barrier(Transition(image, ResourceState::kUndefined, ResourceState::kGeneral));
  });

  f32 light[16] = {0, 1, 0, 10, 1, 1, 1, 0};
  GpuBuffer lights = device->CreateBuffer(sizeof(light), kBufferUsageStorage, true);
  std::vector<f32> cdf(1 + kSkyH + 2 * kSkyW * kSkyH);
  const f32 omega = 2 * kPi / kSkyW * (1 - std::cos(kPi / kSkyH));
  cdf[0] = omega;
  for (u32 r = 0; r < kSkyH; ++r) cdf[1 + r] = omega;
  for (u32 c = 0; c < kSkyW; ++c) cdf[1 + kSkyH + c] = 1;
  cdf[1 + kSkyH + kSkyW * kSkyH] = 1;
  GpuBuffer table = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(cdf.data()), cdf.size() * sizeof(f32)}, kBufferUsageStorage);
  if (!lights.mapped || !table) return 1;
  SamplerHandle sampler = device->GetSampler({});
  std::vector<f32> values[4];
  for (auto& value : values) value.resize(kPixels * 4);
  bool first = true;
  auto run = [&](Push push, f32 point_target, GpuImage prev0 = {}, GpuImage prev1 = {},
                 GpuImage prev2 = {}, GpuImage prev3 = {}) {
    light[7] = point_target / (.99f * .99f);
    std::memcpy(lights.mapped, light, sizeof(light));
    device->FlushBuffer(lights, 0, sizeof(light));
    device->ImmediateSubmit([&](CommandList& cmd) {
      if (!first)
        for (GpuImage& image : out)
          cmd.Barrier(Transition(image, ResourceState::kCopySrc, ResourceState::kGeneral));
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {
          Bind::Storage(0, out[0]), Bind::Storage(1, out[1]), Bind::Sampled(2, position),
          Bind::Sampled(3, nr), Bind::Sampled(4, nr), Bind::Sampled(5, vz), Bind::Sampled(6, vz),
          Bind::Sampled(7, id), Bind::Sampled(8, id), Bind::Sampled(9, motion),
          Bind::Sampled(10, prev0 ? prev0 : empty), Bind::Sampled(11, prev1 ? prev1 : empty),
          Bind::StorageBuffer(12, lights, 0, lights.size), Bind::Combined(13, sky.view, sampler),
          Bind::StorageBuffer(14, table, 0, table.size), Bind::Storage(15, out[2]),
          Bind::Storage(16, out[3]), Bind::Sampled(17, prev2 ? prev2 : empty),
          Bind::Sampled(18, prev3 ? prev3 : empty)});
      cmd.Push(push);
      cmd.Dispatch(kWidth / 8, kHeight / 8, 1);
    });
    for (u32 i = 0; i < 4; ++i)
      check(device->ReadbackImage(out[i], ResourceState::kGeneral, values[i].data(),
                                  values[i].size() * sizeof(f32)), "read back reservoir");
    first = false;
  };

  for (u32 candidates : {1u, 4u, 8u}) {
    for (u32 mode = 0; mode < 3; ++mode) {
      Push push;
      push.candidates = candidates;
      push.sun_direction[3] = mode == 1 ? 0 : 1;
      run(push, mode == 0 ? 0 : 1);
      f32 expected = mode == 2 ? 2 : 1;
      bool correct = true;
      for (u32 p = 0; p < kPixels; ++p)
        correct &= std::abs(values[1][4 * p + 2] - expected) < 1e-4f;
      std::printf("candidates=%u mode=%u W=%g expected=%g\n", candidates, mode,
                   values[1][2], expected);
      check(correct, "candidate count must not change total light energy");
    }
  }
  Push push;
  push.candidates = 0;
  run(push, 1);
  check(std::abs(values[1][2] - 1) < 1e-5f, "zero point proposals retain sun energy");
  push.candidates = 8;
  push.light_count = 0;
  run(push, 0);
  check(std::abs(values[1][2] - 1) < 1e-5f, "sun-only scene retains its energy");
  push.sun_direction[3] = 0;
  push.sky_candidates = 1;
  run(push, 0);
  double mean = 0;
  u32 lower_half = 0, samples = 0;
  const double cap = 1.0 - std::cos(double(kPi) / kSkyH);
  for (u32 p = 0; p < kPixels; ++p) {
    // Direction-to-cell rounding can place boundary samples in a dark neighbor.
    if (values[2][4 * p + 3] > -1.5f) continue;
    double u = (1.0 - values[2][4 * p + 1]) / cap;
    mean += u;
    lower_half += u < .5;
    ++samples;
  }
  mean /= samples ? samples : 1;
  const double fraction = double(lower_half) / (samples ? samples : 1);
  std::printf("sky solid-angle mean=%g lower-half fraction=%g retained=%u/%u\n",
               mean, fraction, samples, kPixels);
  check(samples >= kPixels - 8, "sky proposals must retain positive target mass");
  check(std::abs(mean - .5) < .015, "sky cells must be uniform in solid angle");
  check(std::abs(fraction - .5) < .02, "sky PDF must match cell samples");

  const f32 dead_id[4] = {0, 0, 0, -1}, light_id[4] = {0, 0, 0, 1};
  const f32 dead_mass[4] = {0, 2, 0, 0}, live_mass[4] = {4, 2, 2, 0};
  GpuImage dead_sample = input(Format::kRGBA32Float, dead_id, sizeof(dead_id));
  GpuImage live_sample = input(Format::kRGBA32Float, light_id, sizeof(light_id));
  GpuImage dead_history = input(Format::kRGBA32Float, dead_mass, sizeof(dead_mass));
  GpuImage live_history = input(Format::kRGBA32Float, live_mass, sizeof(live_mass));
  Push reuse;
  reuse.sun_direction[3] = 0;
  reuse.candidates = 1;
  reuse.reset = 0;
  run(reuse, 1, dead_sample, dead_history);
  const f32 dark_estimate = values[1][2];
  check(std::abs(values[1][1] - 4) < 1e-5f, "zero-weight DI history retains sample count");
  run(reuse, 1, live_sample, live_history);
  const f32 bright_estimate = values[1][2];
  std::printf("DI reuse: dark=%g bright=%g mean=%g expected=1\n",
               dark_estimate, bright_estimate, (dark_estimate + bright_estimate) / 2);
  check(std::abs((dark_estimate + bright_estimate) / 2 - 1) < 1e-5f,
        "equally likely zero and double-energy histories must preserve expected energy");
  reuse.pad[1] = 1;
  run(reuse, 1, live_sample, live_history);
  check(values[1][1] == 4 && values[1][4 * (kWidth - 1) + 1] == 2,
        "DI reprojection includes the previous-minus-current jitter delta");
  reuse.light_count = 0;
  reuse.sky_candidates = 1;
  run(reuse, 0, {}, {}, dead_sample, dead_history);
  check(std::abs(values[3][1] - 3) < 1e-5f, "zero-weight sky history retains sample count");
  check(values[3][4 * (kWidth - 1) + 1] == 1,
        "sky reservoir reprojection includes the jitter delta");

  GpuImage half_out = device->CreateImage2D(Format::kRGBA16Float, {kWidth, kHeight},
      kTextureUsageStorage | kTextureUsageTransferSrc);
  GpuImage shaded = device->CreateImage2D(Format::kRGBA16Float, {kWidth, kHeight}, kTextureUsageStorage);
  if (!half_out || !shaded) return 1;
  device->ImmediateSubmit([&](CommandList& cmd) {
    for (GpuImage& image : out)
      cmd.Barrier(Transition(image, ResourceState::kCopySrc, ResourceState::kGeneral));
    cmd.Barrier(Transition(half_out, ResourceState::kUndefined, ResourceState::kGeneral));
    cmd.Barrier(Transition(shaded, ResourceState::kUndefined, ResourceState::kGeneral));
  });
  auto count_stage = [&](ShaderBlob shader, const auto& constants,
                         std::initializer_list<BindingItem> bindings, GpuImage mass, u32 channel,
                         const char* name, BindlessRegistry* bindless = nullptr) {
    ComputePipelineDesc stage_desc;
    stage_desc.shader = shader;
    stage_desc.sets.resize(bindless ? 2 : 1);
    for (const auto& binding : bindings)
      stage_desc.sets[0].slots.push_back({binding.slot, binding.type});
    if (bindless) stage_desc.sets[1].shared = bindless->set_layout();
    stage_desc.push_constant_size = sizeof(constants);
    PipelineHandle stage = device->CreateComputePipeline(stage_desc);
    if (!stage) { check(false, name); return; }
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeWrite);
      cmd.BindPipeline(stage);
      cmd.BindTransient(0, bindings);
      if (bindless) cmd.BindSet(1, bindless->set());
      cmd.Push(constants);
      cmd.Dispatch(kWidth / 8, kHeight / 8, 1);
    });
    const u32 center = 4 * ((kHeight / 2) * kWidth + kWidth / 2) + channel;
    if (mass.format == Format::kRGBA16Float) {
      std::vector<u16> data(kPixels * 4);
      check(device->ReadbackImage(mass, ResourceState::kGeneral, data.data(), data.size() * sizeof(u16)),
            "read half reservoir count");
      std::printf("%s: M half bits=%04x\n", name, data[center]);
      check(data[center] > 0x4000 && data[center] < 0x7c00, name);
    } else {
      check(device->ReadbackImage(mass, ResourceState::kGeneral, values[0].data(), values[0].size() * sizeof(f32)),
            "read reservoir count");
      std::printf("%s: M=%g\n", name, values[0][center]);
      check(values[0][center] > 2 && std::isfinite(values[0][center]), name);
    }
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(mass, ResourceState::kCopySrc, ResourceState::kGeneral));
    });
    device->DestroyPipeline(stage);
  };
  const f32 sample_position[4] = {0, 1, 0, 0};
  const f32 sample_radiance[4] = {1, 1, 1, 0};
  const f32 gi_mass[4] = {.5f, 1, .5f, 2};
  GpuImage gi_pos = input(Format::kRGBA32Float, sample_position, sizeof(sample_position));
  GpuImage gi_rad = input(Format::kRGBA32Float, sample_radiance, sizeof(sample_radiance));
  GpuImage gi_history = input(Format::kRGBA32Float, gi_mass, sizeof(gi_mass));
  struct GiTemporalPush {
    u32 size[2] = {kWidth, kHeight};
    u32 frame_index = 0;
    f32 m_max = 20;
    f32 reset = 0;
    f32 pad[3] = {};
  } gi_temporal;
  gi_temporal.pad[1] = 1;
  count_stage(RX_SHADER(k_recon_restir_temporal_cs_hlsl), gi_temporal, {
      Bind::Storage(0, out[0]), Bind::Storage(1, half_out), Bind::Storage(2, out[1]),
      Bind::Sampled(3, gi_pos), Bind::Sampled(4, nr), Bind::Sampled(5, gi_rad),
      Bind::Sampled(6, position), Bind::Sampled(7, nr), Bind::Sampled(8, nr),
      Bind::Sampled(9, vz), Bind::Sampled(10, vz), Bind::Sampled(11, id), Bind::Sampled(12, id),
      Bind::Sampled(13, motion), Bind::Sampled(14, empty), Bind::Sampled(15, gi_history),
      Bind::Sampled(16, empty), Bind::Sampled(17, position)}, half_out, 3, "zero-weight GI temporal history retains sample count");
  std::vector<u16> jitter_counts(kPixels * 4);
  check(device->ReadbackImage(half_out, ResourceState::kGeneral, jitter_counts.data(),
                              jitter_counts.size() * sizeof(u16)), "read GI jitter reprojection");
  check(jitter_counts[3] == 0x4200 && jitter_counts[4 * (kWidth - 1) + 3] == 0x3c00,
        "GI reprojection includes the previous-minus-current jitter delta");
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(half_out, ResourceState::kCopySrc, ResourceState::kGeneral));
  });
  gi_temporal.pad[1] = 0;

  const f32 shifted_primary[4] = {0, 1, 0, 1}, gi_live[4] = {0, 2, 0, kPi};
  GpuImage previous_primary = input(Format::kRGBA32Float, shifted_primary, sizeof(shifted_primary));
  GpuImage live_gi = input(Format::kRGBA32Float, gi_live, sizeof(gi_live));
  count_stage(RX_SHADER(k_recon_restir_temporal_cs_hlsl), gi_temporal, {
      Bind::Storage(0, out[0]), Bind::Storage(1, half_out), Bind::Storage(2, out[1]),
      Bind::Sampled(3, gi_pos), Bind::Sampled(4, nr), Bind::Sampled(5, gi_rad),
      Bind::Sampled(6, position), Bind::Sampled(7, nr), Bind::Sampled(8, nr),
      Bind::Sampled(9, vz), Bind::Sampled(10, vz), Bind::Sampled(11, id), Bind::Sampled(12, id),
      Bind::Sampled(13, motion), Bind::Sampled(14, live_gi), Bind::Sampled(15, gi_history),
      Bind::Sampled(16, gi_rad), Bind::Sampled(17, previous_primary)}, half_out, 3,
      "GI temporal reconnection retains sample count");
  check(device->ReadbackImage(out[0], ResourceState::kGeneral, values[0].data(), values[0].size() * sizeof(f32)),
        "read GI reconnection weight");
  std::printf("GI reconnection W=%g expected=%g\n", values[0][3], kPi * .5f);
  check(std::abs(values[0][3] - kPi * .5f) < 1e-5f, "temporal GI must convert source solid angle to current solid angle");
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(out[0], ResourceState::kCopySrc, ResourceState::kGeneral));
  });

  if (device->caps().ray_query) {
    auto rt = RayTracingContext::Create(*device);
    auto bindless = BindlessRegistry::Create(*device);
    if (!rt || !bindless) return 1;
    struct DiSpatialPush {
      f32 sun_direction[4] = {0, -1, 0, 1};
      f32 sun_color[4] = {1, 1, 1, 0};
      u32 size[2] = {kWidth, kHeight};
      u32 frame_index = 0, light_count = 1, sample_count = 16;
      f32 radius = 5;
      f32 pad[2] = {};
    } di_spatial;
    count_stage(RX_SHADER(k_recon_restir_di_spatial_cs_hlsl), di_spatial, {
        Bind::Storage(0, shaded), Bind::Sampled(1, dead_sample), Bind::Sampled(2, dead_history),
        Bind::Sampled(3, position), Bind::Sampled(4, nr), Bind::Sampled(5, vz), Bind::Sampled(6, id),
        Bind::StorageBuffer(7, lights), Bind::Accel(8, rt->tlas(0)),
        Bind::Storage(9, out[0]), Bind::Storage(10, out[1]), Bind::Combined(11, sky.view, sampler),
        Bind::StorageBuffer(12, table), Bind::Sampled(13, dead_sample), Bind::Sampled(14, dead_history),
        Bind::Storage(15, out[2]), Bind::Storage(16, out[3])}, out[1], 1,
        "zero-weight DI spatial neighbors retain sample count", bindless.get());
    check(device->ReadbackImage(out[3], ResourceState::kGeneral, values[0].data(), values[0].size() * sizeof(f32)),
          "read spatial sky count");
    check(values[0][4 * ((kHeight / 2) * kWidth + kWidth / 2) + 1] > 2,
          "zero-weight sky spatial neighbors retain sample count");
    struct GiSpatialPush {
      u32 size[2] = {kWidth, kHeight};
      u32 frame_index = 0, sample_count = 16;
      f32 radius = 5;
      u32 debug = 0;
      f32 pad[2] = {};
    } gi_spatial;
    count_stage(RX_SHADER(k_recon_restir_spatial_cs_hlsl), gi_spatial, {
        Bind::Storage(0, shaded), Bind::Sampled(1, empty), Bind::Sampled(2, gi_history),
        Bind::Sampled(3, empty), Bind::Sampled(4, position), Bind::Sampled(5, nr),
        Bind::Sampled(6, vz), Bind::Sampled(7, id), Bind::Sampled(8, empty),
        Bind::Accel(9, rt->tlas(0)), Bind::Storage(10, out[0]), Bind::Storage(11, half_out),
        Bind::Storage(12, out[1])}, half_out, 3,
        "zero-weight GI spatial neighbors retain sample count");
  } else {
    std::printf("path_sampling_test: spatial checks SKIP, ray queries unavailable\n");
  }
  PipelineHandle continuation = device->CreateComputePipeline({
      .shader = RX_SHADER(k_path_continue_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}}});
  if (!continuation) return 1;
  device->ImmediateSubmit([&](CommandList& cmd) {
    cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeWrite);
    cmd.BindPipeline(continuation);
    cmd.BindTransient(0, {Bind::Storage(0, out[0])});
    cmd.Dispatch(kWidth / 8, kHeight / 8, 1);
  });
  check(device->ReadbackImage(out[0], ResourceState::kGeneral, values[0].data(), values[0].size() * sizeof(f32)),
        "read path continuation estimate");
  double energy[3] = {};
  for (u32 p = 0; p < kPixels; ++p)
    for (u32 c = 0; c < 3; ++c) energy[c] += values[0][4 * p + c] / kPixels;
  for (u32 c = 0; c < 3; ++c)
    check(std::abs(energy[c] - .001 * (c + 1)) < .00015, "roulette retains expected energy of dim paths");
  std::printf("dim-path expected energy=(.001,.002,.003), measured=(%g,%g,%g)\n", energy[0], energy[1], energy[2]);
  device->DestroyPipeline(continuation);
  device->DestroyImage(half_out);
  device->DestroyImage(shaded);

  device->WaitIdle();
  for (GpuImage& image : owned) device->DestroyImage(image);
  for (GpuImage& image : out) device->DestroyImage(image);
  device->DestroyImage(sky);
  device->DestroyBuffer(lights);
  device->DestroyBuffer(table);
  device->DestroyPipeline(pipeline);
  std::printf("path_sampling_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
