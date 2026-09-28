#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxe/asset/mesh.h"
#include "rxe/render/core/bindless.h"
#include "shaders/path_alpha_di_cs_hlsl.h"
#include "shaders/path_alpha_recon_cs_hlsl.h"
#ifdef RX_HAS_NRD
#include "base/memory/mem_ops.h"
#include "shaders/path_alpha_nrd_cs_hlsl.h"
#endif

using namespace rx;
using namespace rx::render;
namespace gpu = rx::gpu;

int main() {
  gpu::DeviceDesc desc;
  const char* backend = ::getenv("RX_RHI");
  desc.backend = backend && ::strcmp(backend, "d3d12") == 0
                     ? gpu::Backend::kD3D12 : gpu::Backend::kVulkan;
  desc.enable_validation = true;
  auto device = gpu::Device::CreateOffscreen(desc);
  if (!device || device->is_stub()) {
    ::printf("path_alpha_test: SKIP, GPU unavailable\n");
    return 77;
  }
  auto bindless = BindlessRegistry::Create(*device);
  if (!bindless) return 1;
  asset::Vertex vertices[3] = {{.position = {0, 0, 0}},
                              {.position = {1, 0, 0}},
                              {.position = {0, 1, 0}}};
  const u32 indices[3] = {0, 1, 2};
  gpu::GpuBuffer vb = device->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(vertices), sizeof(vertices)),
      gpu::kBufferUsageStorage | gpu::kBufferUsageDeviceAddress);
  gpu::GpuBuffer ib = device->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(indices), sizeof(indices)),
      gpu::kBufferUsageStorage | gpu::kBufferUsageDeviceAddress);
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
  gpu::GpuBuffer result = device->CreateBuffer(sizeof(expected), gpu::kBufferUsageStorage | gpu::kBufferUsageTransferSrc);
  gpu::GpuBuffer readback = device->CreateBuffer(sizeof(expected), gpu::kBufferUsageTransferDst, true);
  if (!result || !readback.mapped) return 1;
  int failures = 0;
  auto run = [&](gpu::ShaderBlob shader, const char* name) {
    gpu::PipelineHandle pipeline = device->CreateComputePipeline({
        .shader = shader,
        .sets = {{.slots = {{31, gpu::BindingType::kStorageBuffer}}}, {.shared = bindless->set_layout()}},
        .debug_name = name});
    if (!pipeline) { ++failures; return; }
    device->ImmediateSubmit([&](gpu::CommandList& cmd) {
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {gpu::Bind::StorageBuffer(31, result)});
      cmd.BindSet(1, bindless->set());
      cmd.Dispatch(1, 1, 1);
      cmd.MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kTransferRead);
      cmd.CopyBuffer(result, 0, readback, 0, sizeof(expected));
      cmd.MemoryBarrier(gpu::BarrierScope::kTransferRead, gpu::BarrierScope::kComputeWrite);
    });
    device->InvalidateBuffer(readback, 0, sizeof(expected));
    u32 actual[8];
    base::MemCopy(actual, readback.mapped, sizeof(actual));
    for (u32 i = 0; i < 8; ++i) {
      if (actual[i] == expected[i]) continue;
      ::printf("FAIL: %s alpha=%g cutoff=%g got=%u expected=%u\n",
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
  bindless.Reset();
  device->DestroyBuffer(vb);
  device->DestroyBuffer(ib);
  device->DestroyBuffer(result);
  device->DestroyBuffer(readback);
  ::printf("path_alpha_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
