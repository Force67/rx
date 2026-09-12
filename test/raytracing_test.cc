#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "asset/mesh.h"
#include "render/gi/raytracing.h"
#include "render/gi/skinned_rt.h"
#include "render/pipeline/material_system.h"
#include "render/rhi/device.h"
#include "shaders/rt_query_cs_hlsl.h"

using namespace rx;
using namespace rx::render;

int main() {
  DeviceDesc desc;
  const char* backend = std::getenv("RX_RHI");
  desc.backend = backend && std::strcmp(backend, "d3d12") == 0
                     ? Backend::kD3D12 : Backend::kVulkan;
  desc.request_raytracing = true;
  desc.enable_validation = true;
  auto device = Device::CreateOffscreen(desc);
  if (!device || device->is_stub() || !device->caps().ray_query) {
    std::printf("raytracing_test: SKIP, ray queries unavailable\n");
    return 0;
  }
  int failures = 0;
  auto check = [&](bool ok, const char* message) {
    if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
  };
  auto rt = RayTracingContext::Create(*device);
  if (!rt) return 1;
  PipelineHandle pipeline = device->CreateComputePipeline({
      .shader = RX_SHADER(k_rt_query_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kAccelStruct}, {1, BindingType::kStorageBuffer}}}},
      .debug_name = "rt_query_test"});
  GpuBuffer result = device->CreateBuffer(sizeof(f32), kBufferUsageStorage | kBufferUsageTransferSrc);
  GpuBuffer readback = device->CreateBuffer(sizeof(f32), kBufferUsageTransferDst, true);
  if (!pipeline || !result || !readback.mapped) return 1;
  auto trace = [&](u32 slot) {
    device->ImmediateSubmit([&](CommandList& cmd) {
      cmd.BindPipeline(pipeline);
      cmd.BindTransient(0, {Bind::Accel(0, rt->tlas(slot)), Bind::StorageBuffer(1, result)});
      cmd.Dispatch(1, 1, 1);
      cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kTransferRead);
      cmd.CopyBuffer(result, 0, readback, 0, sizeof(f32));
    });
    device->InvalidateBuffer(readback, 0, sizeof(f32));
    f32 distance;
    std::memcpy(&distance, readback.mapped, sizeof(distance));
    return distance;
  };
  check(trace(0) == -1.0f, "unbuilt slot must miss through fallback");

  asset::Vertex vertices[3] = {{.position = {-1, -1, 0}},
                              {.position = {1, -1, 0}},
                              {.position = {0, 1, 0}}};
  const u32 indices[3] = {0, 1, 2};
  GpuMesh mesh;
  mesh.vertices = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(vertices), sizeof(vertices)},
      kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress | kBufferUsageStorage);
  mesh.indices = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(indices), sizeof(indices)},
      kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress | kBufferUsageStorage);
  mesh.vertex_count = mesh.index_count = 3;
  mesh.submeshes.push_back({.index_count = 3});
  check(rt->BuildBlas(1, mesh), "build compacted mesh BLAS");
  base::Vector<RayTracingContext::Instance> instances;
  instances.push_back({.mesh_key = 1, .transform = Mat4::Identity()});
  auto build = [&](u32 frame) {
    check(rt->ReserveTlas(0, static_cast<u32>(instances.size())), "reserve TLAS");
    device->ImmediateSubmit([&](CommandList& cmd) { rt->BuildTlas(cmd, 0, frame, instances); });
  };
  build(0);
  check(std::abs(trace(0) - 1.0f) < 1e-5f, "compacted triangle must hit at distance 1");
  check(rt->ReserveTlas(0, 65), "grow built TLAS");
  check(!rt->TlasValid(0), "replacement remains invalid until built");
  check(trace(0) == -1.0f, "replacement must use fallback before build");
  build(1);
  check(std::abs(trace(0) - 1.0f) < 1e-5f, "grown TLAS must hit after build");
  rt->RemoveBlasDeferred(1);
  check(trace(0) == -1.0f, "retired BLAS must invalidate TLAS");
  build(2);
  check(trace(0) == -1.0f, "removed instance must be omitted from rebuilt TLAS");

  base::Vector<AccelTriangles> geometry;
  geometry.push_back({.vertex_address = mesh.vertices.address,
                       .vertex_stride = sizeof(asset::Vertex), .vertex_count = 3,
                       .vertex_format = Format::kRGB32Float,
                       .index_address = mesh.indices.address, .index_count = 3});
  check(rt->ReserveSkinnedBlas(2, geometry), "reserve unbuilt skinned BLAS");
  instances[0].mesh_key = 2;
  instances[0].skinned = true;
  build(3);
  check(trace(0) == -1.0f, "unbuilt skinned BLAS must not enter TLAS");
  device->ImmediateSubmit([&](CommandList& cmd) {
    rt->RecordSkinnedBlas(cmd, 2, 0);
    cmd.MemoryBarrier(BarrierScope::kAccelBuildWrite, BarrierScope::kAccelBuildWrite);
    rt->BuildTlas(cmd, 0, 4, instances);
  });
  check(std::abs(trace(0) - 1.0f) < 1e-5f, "built skinned BLAS must hit");
  check(rt->ReserveSkinnedBlas(3, geometry), "reserve refit target");
  instances[0].mesh_key = 3;
  device->ImmediateSubmit([&](CommandList& cmd) {
    rt->RecordSkinnedBlas(cmd, 3, 2);
    cmd.MemoryBarrier(BarrierScope::kAccelBuildWrite, BarrierScope::kAccelBuildWrite);
    rt->BuildTlas(cmd, 0, 5, instances);
  });
  check(std::abs(trace(0) - 1.0f) < 1e-5f, "refitted BLAS must hit");

  rt->RemoveSkinnedBlasDeferred(2);
  rt->RemoveSkinnedBlasDeferred(3);
  auto bindless = BindlessRegistry::Create(*device);
  auto materials = MaterialSystem::Create(*device, bindless.get());
  SkinnedRayTracing skin;
  if (!bindless || !materials || !skin.Initialize(*device)) return 1;
  asset::SkinnedVertexExtra weights[3]{};
  for (auto& weight : weights) weight.bone_weights[0] = 255;
  mesh.skinning = device->CreateBufferWithData(
      {reinterpret_cast<const u8*>(weights), sizeof(weights)}, kBufferUsageStorage);
  mesh.skinned = true;
  GpuBuffer bones = device->CreateBuffer(sizeof(Mat4), kBufferUsageStorage, true);
  if (!mesh.skinning || !bones.mapped) return 1;
  base::UnorderedMap<rx::u64, GpuMesh> meshes;
  meshes.emplace(7, mesh);
  const u32 actor = skin.Acquire();
  base::Vector<SkinnedRayTracing::Request> requests;
  requests.push_back({.handle = actor, .mesh_key = 7});
  auto pose = [&](f32 z, u32 frame) {
    Mat4 bone = MakeTranslation(Vec3{0, 0, z});
    std::memcpy(bones.mapped, &bone, sizeof(bone));
    device->FlushBuffer(bones, 0, sizeof(bone));
    check(skin.Prepare(*device, *bindless, *materials, *rt, meshes, requests) == 1,
          "prepare posed actor");
    instances[0].mesh_key = skin.blas_key(actor);
    instances[0].custom_index = skin.custom_index(actor);
    device->ImmediateSubmit([&](CommandList& cmd) {
      skin.Record(cmd, *rt, bones);
      rt->BuildTlas(cmd, 0, frame, instances);
    });
    check(std::abs(trace(0) - (z + 1.0f)) < 1e-5f, "ray must hit the current skinned pose");
  };
  pose(1, 6);
  check(skin.previous_custom_index(actor) == SkinnedRayTracing::kInvalidIndex,
        "first skin pose must not expose uninitialized previous vertices");
  skin.BeginFrame();
  check(!skin.active(actor), "an omitted actor must not retain last frame's active state");
  check(skin.custom_index(actor) == SkinnedRayTracing::kInvalidIndex,
        "an omitted actor must not expose stale geometry");
  pose(2, 7);
  check(skin.previous_custom_index(actor) != SkinnedRayTracing::kInvalidIndex &&
        skin.previous_custom_index(actor) != skin.custom_index(actor),
        "second skin pose must expose the previous buffer for motion");
  base::Vector<u32> retired;
  skin.InvalidateMesh(*device, rt.get(), 7, retired);
  check(!skin.active(actor) && !rt->TlasValid(0), "mesh replacement must retire posed geometry");
  device->WaitIdle();
  for (u32 index : retired) bindless->ReleaseMesh(index);
  retired.clear();
  pose(3, 8);
  skin.Release(*device, rt.get(), actor, retired);
  skin.Release(*device, rt.get(), actor, retired);
  const u32 reused = skin.Acquire();
  check(reused == actor && skin.Acquire() != reused, "duplicate release must not alias live actors");
  device->WaitIdle();
  for (u32 index : retired) bindless->ReleaseMesh(index);
  skin.Destroy(*device);
  materials.reset();
  bindless.reset();
  device->DestroyBuffer(mesh.skinning);
  device->DestroyBuffer(bones);

  device->WaitIdle();
  rt.reset();
  device->DestroyBuffer(mesh.vertices);
  device->DestroyBuffer(mesh.indices);
  device->DestroyBuffer(result);
  device->DestroyBuffer(readback);
  device->DestroyPipeline(pipeline);
  std::printf("raytracing_test: %d failures\n", failures);
  return failures ? 1 : 0;
}
