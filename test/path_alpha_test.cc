#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "asset/mesh.h"
#include "render/core/bindless.h"
#include "shaders/path_alpha_recon_cs_hlsl.h"
#include "shaders/path_alpha_di_cs_hlsl.h"
#ifdef RX_HAS_NRD
#include "shaders/path_alpha_nrd_cs_hlsl.h"
#endif

using namespace rx;
using namespace rx::render;

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0
                     ? Backend::kD3D12 : Backend::kVulkan;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    std::printf("path_alpha_test: SKIP, GPU unavailable\n");
    return 0;
  }
  auto bindless = BindlessRegistry::Create(*device);
  if (!bindless) return 1;
  asset::Vertex vertices[3] = {{.position = {0, 0, 0}},
                              {.position = {1, 0, 0}},
                              {.position = {0, 1, 0}}};
  const u32 indices[3] = {0, 1, 2};
  GpuBuffer vb = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(vertices), sizeof(vertices)},
      kBufferUsageStorage | kBufferUsageDeviceAddress);
  GpuBuffer ib = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(indices), sizeof(indices)},
      kBufferUsageStorage | kBufferUsageDeviceAddress);
  const f32 alphas[8] = {0, .49f, .5f, 1, 0, 0, .75f, .75f};
  const f32 cutoffs[8] = {.5f, .5f, .5f, .5f, .5f, 0, .8f, .7f};
  const u32 expected[8] = {0, 0, 1, 1, 1, 1, 0, 1};
  BindlessRegistry::GeometryRecord geometry[8];
  for (u32 i = 0; i < 8; ++i) {
    BindlessRegistry::MaterialRecord material;
    material.flags = i == 4 ? 0 : BindlessRegistry::kMaterialAlphaMask;
    material.base_color_factor[3] = alphas[i];
    material.alpha_cutoff = cutoffs[i];
    geometry[i].material_index = bindless->RegisterMaterial(material);
  }
  if (!vb || !ib || bindless->RegisterMesh(vb, ib, geometry, 8) != 0) return 1;
  GpuBuffer result = device->CreateBuffer(sizeof(expected), kBufferUsageStorage | kBufferUsageTransferSrc);
  GpuBuffer readback = device->CreateBuffer(sizeof(expected), kBufferUsageTransferDst, true);
  if (!result || !readback.mapped) return 1;
  int failures = 0;
  auto run = [&](ShaderBlob shader, const char* name) {
    PipelineHandle pipeline = device->CreateComputePipeline({
        .shader = shader,
        .sets = {{.slots = {{31, BindingType::kStorageBuffer}}}, {.shared = bindless->set_layout()}},
        .debug_name = name});
    if (!pipeline) { ++failures; return; }
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {Bind::StorageBuffer(31, result)});
      cmd.BindSet(1, bindless->set());
      cmd.Dispatch(1, 1, 1);
      cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kTransferRead);
      cmd.CopyBuffer(result, 0, readback, 0, sizeof(expected));
      cmd.MemoryBarrier(BarrierScope::kTransferRead, BarrierScope::kComputeWrite);
    });
    device->InvalidateBuffer(readback, 0, sizeof(expected));
    u32 actual[8];
    std::memcpy(actual, readback.mapped, sizeof(actual));
    for (u32 i = 0; i < 8; ++i) {
      if (actual[i] == expected[i]) continue;
      std::printf("FAIL: %s alpha=%g cutoff=%g got=%u expected=%u\n",
                  name, alphas[i], cutoffs[i], actual[i], expected[i]);
      ++failures;
    }
    device->DestroyPipeline(pipeline);
  };
  run(RX_SHADER(k_path_alpha_recon_cs_hlsl), "recon");
  run(RX_SHADER(k_path_alpha_di_cs_hlsl), "di shadow");
#ifdef RX_HAS_NRD
  run(RX_SHADER(k_path_alpha_nrd_cs_hlsl), "nrd");
#endif
  device->WaitIdle();
  bindless.reset();
  device->DestroyBuffer(vb);
  device->DestroyBuffer(ib);
  device->DestroyBuffer(result);
  device->DestroyBuffer(readback);
  std::printf("path_alpha_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
