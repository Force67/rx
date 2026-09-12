#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "asset/mesh.h"
#include "render/core/bindless.h"
#include "render/gi/path_scene_history.h"
#include "render/gi/skinned_rt.h"
#include "render/rhi/device.h"
#include "shaders/path_motion_recon_cs_hlsl.h"
#ifdef RX_HAS_NRD
#include "shaders/path_motion_nrd_cs_hlsl.h"
#endif

using namespace rx;
using namespace rx::render;

int main() {
  int failures = 0;
  auto check = [&](bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
  };
  PathSceneHistory history;
  base::Vector<RayTracingContext::Instance> instances(2);
  instances[0].mesh_key = 1;
  instances[0].custom_index = instances[0].previous_mesh = 7;
  instances[1] = instances[0];
  instances[1].transform = instances[1].previous_transform = MakeTranslation({1, 0, 0});
  check(history.Update(instances, {}), "first scene invalidates reference accumulation");
  const u32 first = instances[0].history_id, second = instances[1].history_id;
  check(first != second && instances[0].previous_mesh == 0xffffffffu, "new instances have distinct identities and no history");
  for (auto& instance : instances) instance.previous_mesh = instance.custom_index;
  check(!history.Update(instances, {}), "static scene retains accumulation");
  check(instances[0].history_id == first, "static instance retains identity");
  instances[0].transform = MakeTranslation({0, 1, 0});
  check(history.Update(instances, {}), "rigid motion resets reference accumulation");
  check(instances[0].history_id == first && instances[0].previous_mesh == 7,
        "rigid motion retains valid surface history");
  instances[0].previous_transform = instances[0].transform;
  std::swap(instances[0], instances[1]);
  check(history.Update(instances, {}), "draw reorder resets reference accumulation");
  check(instances[0].history_id != first && instances[0].history_id != second &&
        instances[0].previous_mesh == 0xffffffffu, "reordered instances cannot inherit another object's history");
  Mat4 bone = Mat4::Identity();
  history.Update(instances, {&bone, 1});
  bone = MakeTranslation({0, 0, 1});
  check(history.Update(instances, {&bone, 1}), "pose changes reset reference accumulation");
  instances.clear();
  check(history.Update(instances, {}), "removed geometry resets reference accumulation");

  DeviceDesc desc;
  desc.enable_validation = true;
  desc.request_raytracing = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub() || !device->caps().ray_query) {
    std::printf("path_motion_test: GPU checks SKIP, %d CPU failures\n", failures);
    return failures ? 1 : 77;
  }
  auto rt = RayTracingContext::Create(*device);
  auto bindless = BindlessRegistry::Create(*device);
  if (!rt || !bindless) return 1;
  asset::Vertex vertices[3] = {{.position = {-1, -1, 0}, .normal = {0, 0, -1}},
                              {.position = {1, -1, 0}, .normal = {0, 0, -1}},
                              {.position = {0, 1, 0}, .normal = {0, 0, -1}}};
  const u32 indices[3] = {0, 1, 2};
  GpuMesh mesh;
  mesh.vertices = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(vertices), sizeof(vertices)},
      kBufferUsageStorage | kBufferUsageDeviceAddress | kBufferUsageAccelBuildInput);
  mesh.indices = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(indices), sizeof(indices)},
      kBufferUsageStorage | kBufferUsageDeviceAddress | kBufferUsageAccelBuildInput);
  mesh.vertex_count = mesh.index_count = 3;
  mesh.submeshes.push_back({.index_count = 3});
  BindlessRegistry::GeometryRecord geometry;
  geometry.material_index = bindless->RegisterMaterial({});
  const u32 current_mesh = bindless->RegisterMesh(mesh.vertices, mesh.indices, &geometry, 1);
  for (auto& vertex : vertices) vertex.position[0] -= .25f;
  GpuBuffer previous_vertices = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(vertices), sizeof(vertices)}, kBufferUsageStorage | kBufferUsageDeviceAddress);
  const u32 previous_mesh = bindless->RegisterMesh(previous_vertices, mesh.indices, &geometry, 1);
  if (!rt->BuildBlas(1, mesh) || !rt->ReserveTlas(0, 2)) return 1;
  GpuBuffer result = device->CreateBuffer(8 * sizeof(f32), kBufferUsageStorage | kBufferUsageTransferSrc);
  GpuBuffer readback = device->CreateBuffer(8 * sizeof(f32), kBufferUsageTransferDst, true);
  if (!result || !readback.mapped) return 1;
  auto run = [&](ShaderBlob shader, u32 tlas_binding, u32 motion_binding, bool recon,
                 u32 previous, f32 expected_x, bool valid, f32 translation_x = -.25f) {
    PipelineHandle pipeline = device->CreateComputePipeline({.shader = shader,
        .sets = {{.slots = {{tlas_binding, BindingType::kAccelStruct},
                            {motion_binding, BindingType::kStorageBuffer},
                            {31, BindingType::kStorageBuffer}}}, {.shared = bindless->set_layout()}}});
    if (!pipeline) { check(false, "motion pipeline creation"); return; }
    instances.resize(2);
    instances[0] = {.mesh_key = 999, .history_id = 123};
    instances[1] = {.mesh_key = 1, .custom_index = current_mesh,
                    .previous_transform = MakeTranslation({translation_x, .5f, 0}),
                    .previous_mesh = previous, .history_id = 42};
    device->ImmediateSubmit([&](CommandList& cmd) {
      rt->BuildTlas(cmd, 0, 0, instances);
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {Bind::Accel(tlas_binding, rt->tlas(0)),
                            Bind::StorageBuffer(motion_binding, rt->motion_buffer(0)),
                            Bind::StorageBuffer(31, result)});
      cmd.BindSet(1, bindless->set());
      cmd.Dispatch(1, 1, 1);
      cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kTransferRead);
      cmd.CopyBuffer(result, 0, readback, 0, 8 * sizeof(f32));
      cmd.MemoryBarrier(BarrierScope::kTransferRead, BarrierScope::kComputeWrite);
    });
    device->InvalidateBuffer(readback, 0, 8 * sizeof(f32));
    f32 data[8];
    std::memcpy(data, readback.mapped, sizeof(data));
    std::printf("%s previous=%u pos=(%g,%g,%g) motion=(%g,%g) id=%g\n",
                 recon ? "recon" : "nrd", previous, data[0], data[1], data[2], data[4], data[5], data[6]);
    check(data[7] == 1 && data[3] == (valid ? 1 : 0), "trace hit and motion validity");
    if (valid) {
      check(std::abs(data[0] - expected_x) < 1e-5f && std::abs(data[1] - .5f) < 1e-5f,
            "previous position includes rigid transform and previous vertex pose");
      check(std::abs(data[4] - std::clamp(expected_x * .5f, -2.f, 2.f)) < 1e-5f && std::abs(data[5] - .25f) < 1e-5f,
            "motion is current-to-previous in UV units");
    } else {
      check(data[4] == 2 && data[5] == 2, "missing surface history rejects reprojection");
    }
    if (recon) check(data[6] == 42, "history metadata follows filtered TLAS order");
    device->DestroyPipeline(pipeline);
  };
  for (u32 previous : {current_mesh, previous_mesh, 0xffffffffu}) {
    const f32 x = previous == current_mesh ? -.25f : -.5f;
    run(RX_SHADER(k_path_motion_recon_cs_hlsl), 7, 18, true, previous, x, previous != 0xffffffffu);
#ifdef RX_HAS_NRD
    run(RX_SHADER(k_path_motion_nrd_cs_hlsl), 6, 9, false, previous, x, previous != 0xffffffffu);
#endif
  }
  run(RX_SHADER(k_path_motion_recon_cs_hlsl), 7, 18, true, current_mesh, 1e6f, true, 1e6f);
#ifdef RX_HAS_NRD
  run(RX_SHADER(k_path_motion_nrd_cs_hlsl), 6, 9, false, current_mesh, 1e6f, true, 1e6f);
#endif
  device->WaitIdle();
  rt.reset();
  bindless.reset();
  device->DestroyBuffer(mesh.vertices);
  device->DestroyBuffer(mesh.indices);
  device->DestroyBuffer(previous_vertices);
  device->DestroyBuffer(result);
  device->DestroyBuffer(readback);
  std::printf("path_motion_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
