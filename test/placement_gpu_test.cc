// GPU procedural placement acceptance: runs the DENSITYMAP -> GENERATE ->
// PLACEMENT compute pipeline on an offscreen device and checks the harvested
// instances against the CPU reference generator (same WorldData, same seeds,
// same tiles), plus GPU-vs-GPU determinism across a second run. Skips
// cleanly (exit 0) when no driver is present; run under vkrun for the real
// GPU path. The backend follows RX_RHI (vulkan|d3d12).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "placement/ecotope.h"
#include "placement/gpu_placement.h"
#include "placement/placement.h"
#include "placement/world_data.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

namespace {

using namespace rx;
using namespace rx::placement;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "placement_gpu_test: FAIL: %s\n", message);
  ++failures;
}

// Sort key for order-independent comparison (GPU append order is unstable).
rx::u64 InstanceKey(const PlacedInstance& instance) {
  return (static_cast<rx::u64>(static_cast<u32>(instance.tile_x & 0xffff)) << 48) |
         (static_cast<rx::u64>(static_cast<u32>(instance.tile_z & 0xffff)) << 32) |
         (static_cast<rx::u64>(instance.layer) << 16) | instance.point;
}

void SortInstances(base::Vector<PlacedInstance>& instances) {
  // Insertion sort; the sets are small (a few thousand).
  for (u32 i = 1; i < instances.size(); ++i) {
    PlacedInstance value = instances[i];
    rx::u64 key = InstanceKey(value);
    i32 j = static_cast<i32>(i) - 1;
    while (j >= 0 && InstanceKey(instances[j]) > key) {
      instances[j + 1] = instances[j];
      --j;
    }
    instances[j + 1] = value;
  }
}

}  // namespace

int main() {
  render::DeviceDesc desc;
  const char* rhi = std::getenv("RX_RHI");
  desc.backend = (rhi && std::strcmp(rhi, "d3d12") == 0) ? render::Backend::kD3D12
                                                         : render::Backend::kVulkan;
  desc.request_raytracing = false;
  std::unique_ptr<render::Device> device = render::Device::CreateOffscreen(desc);
  if (!device) {
    std::fprintf(stderr, "placement_gpu_test: CreateOffscreen returned null\n");
    return 1;
  }
  if (device->is_stub()) {
    std::printf("placement_gpu_test: no %s driver, skipping (null backend)\n",
                render::BackendName(desc.backend));
    return 0;
  }
  std::printf("placement_gpu_test: device '%s'\n", device->caps().adapter_name.c_str());

  // A small world with smooth maps (smooth densities keep the CPU/GPU
  // bilinear paths away from threshold knife edges).
  WorldData world(0.0f, 0.0f, 512.0f, 128);
  u32 height = world.AddMap("height");
  world.Generate(height, [](f32 x, f32 z) {
    return 6.0f * std::sin(x * 0.011f) + 4.0f * std::cos(z * 0.017f);
  });
  u32 forest = world.AddMap("forest");
  world.Generate(forest, [](f32 x, f32 z) {
    return 0.5f + 0.5f * std::sin(x * 0.007f) * std::cos(z * 0.009f);
  });

  PlacementConfig config;
  config.jitter = 0.3f;
  PlacementSystem system(&world, height, config);
  Ecotope ecotope;
  {
    PlacementLayer pine;
    pine.name = "pine";
    pine.footprint = 4.0f;
    pine.density.Map(forest).Const(0.6f).Mul();
    pine.scale_min = 0.8f;
    pine.scale_max = 1.2f;
    pine.tilt = 0.25f;
    ecotope.layers.push_back(pine);
  }
  {
    PlacementLayer fir;
    fir.name = "fir";
    fir.footprint = 4.0f;
    fir.density.Map(forest).OneMinus().Const(0.4f).Mul();
    ecotope.layers.push_back(fir);
  }
  {
    PlacementLayer grass;
    grass.name = "grass";
    grass.footprint = 1.0f;
    grass.density.Noise(24.0f, 3.0f);
    ecotope.layers.push_back(grass);
  }
  system.AddEcotope(ecotope);
  system.Compile();

  GpuPlacement gpu;
  if (!gpu.Initialize(*device, system)) {
    std::fprintf(stderr, "placement_gpu_test: Initialize failed\n");
    return 1;
  }
  gpu.SyncWorldData(*device, world);

  const TileKey tiles[] = {{0, 0, 0}, {0, 1, 0}, {0, -1, 2}, {1, 3, 5}, {1, -2, -2}};

  base::Vector<PlacedInstance> gpu_a;
  gpu.GenerateImmediate(*device, system, {tiles, 5}, gpu_a);
  base::Vector<PlacedInstance> gpu_b;
  gpu.GenerateImmediate(*device, system, {tiles, 5}, gpu_b);

  base::Vector<PlacedInstance> cpu;
  for (const TileKey& tile : tiles) system.EmitTileCpu(tile, cpu);

  Check(!gpu_a.empty(), "GPU produced instances");
  Check(gpu_a.size() == gpu_b.size(), "GPU rerun reproduces the count");

  SortInstances(gpu_a);
  SortInstances(gpu_b);
  SortInstances(cpu);

  bool reruns_identical = gpu_a.size() == gpu_b.size();
  for (u32 i = 0; reruns_identical && i < gpu_a.size(); ++i) {
    reruns_identical = std::memcmp(&gpu_a[i], &gpu_b[i], sizeof(PlacedInstance)) == 0;
  }
  Check(reruns_identical, "GPU rerun reproduces instances bit for bit");

  std::printf("placement_gpu_test: gpu=%u cpu=%u instances\n",
              static_cast<u32>(gpu_a.size()), static_cast<u32>(cpu.size()));
  Check(gpu_a.size() == cpu.size(), "GPU matches CPU reference count");
  if (gpu_a.size() == cpu.size()) {
    u32 mismatches = 0;
    for (u32 i = 0; i < gpu_a.size(); ++i) {
      if (InstanceKey(gpu_a[i]) != InstanceKey(cpu[i])) {
        ++mismatches;
        continue;
      }
      for (u32 e = 0; e < 16; ++e) {
        if (std::fabs(gpu_a[i].transform.m[e] - cpu[i].transform.m[e]) > 2e-3f) {
          ++mismatches;
          break;
        }
      }
    }
    Check(mismatches == 0, "GPU instances match the CPU reference");
    if (mismatches != 0) {
      std::fprintf(stderr, "placement_gpu_test: %u mismatching instances\n", mismatches);
    }
  }

  gpu.Shutdown(*device);
  if (failures == 0) std::printf("placement_gpu_test: all checks passed\n");
  return failures;
}
