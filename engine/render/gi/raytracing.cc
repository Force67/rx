#include "render/gi/raytracing.h"

#include <algorithm>
#include <cstring>

#include "asset/mesh.h"
#include "core/log.h"
#include "render/rhi/device.h"

namespace rx::render {
namespace {

u64 AlignUp(u64 value, u64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

void ToInstanceTransform(const Mat4& m, f32 out[3][4]) {
  // TlasInstance transforms are row major 3x4, Mat4 is column major.
  for (u32 row = 0; row < 3; ++row) {
    for (u32 col = 0; col < 4; ++col) {
      out[row][col] = m.m[col * 4 + row];
    }
  }
}

// One geometry per opaque submesh: hit shaders resolve the material from
// CommittedGeometryIndex through the bindless geometry table, which is
// written in the same order. Blend submeshes stay out entirely. Alpha-masked
// (cutout) submeshes go in non-opaque so a ray query can alpha-test them; the
// realtime paths trace RAY_FLAG_FORCE_OPAQUE which overrides that to opaque.
base::Vector<AccelTriangles> BlasGeometries(const GpuMesh& mesh) {
  base::Vector<AccelTriangles> geometries;
  for (const GpuSubmesh& submesh : mesh.submeshes) {
    if (submesh.blend || submesh.index_count == 0) continue;
    geometries.push_back({.vertex_address = mesh.vertices.address,
                          .vertex_stride = sizeof(asset::Vertex),
                          .vertex_count = mesh.vertex_count,
                          .vertex_format = Format::kRGB32Float,
                          .index_address = mesh.indices.address + submesh.index_offset * sizeof(u32),
                          .index_count = submesh.index_count,
                          .index_type = IndexType::kUint32,
                          .opaque = !submesh.alpha_mask});
  }
  return geometries;
}

}  // namespace

std::unique_ptr<RayTracingContext> RayTracingContext::Create(Device& device) {
  auto context = std::unique_ptr<RayTracingContext>(new RayTracingContext(device));
  if (!context->EnsureTlasCapacity(context->fallback_tlas_, 1)) {
    RX_ERROR("fallback tlas allocation failed");
    return nullptr;
  }
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.BuildTlas(context->fallback_tlas_.handle, context->fallback_tlas_.instances, 0,
                  context->fallback_tlas_.scratch);
    cmd.MemoryBarrier(BarrierScope::kAccelBuildWrite, BarrierScope::kAccelRead);
  });
  return context;
}

RayTracingContext::~RayTracingContext() {
  for (auto kv : blas_) device_.DestroyAccelStruct(kv.value.handle);
  for (auto kv : approx_blas_) device_.DestroyAccelStruct(kv.value.handle);
  for (auto kv : lod_blas_)
    for (Blas& blas : kv.value)
      if (blas.handle) device_.DestroyAccelStruct(blas.handle);
  device_.DestroyBuffer(blas_scratch_);
  for (Tlas& tlas : tlas_) DestroyTlas(tlas);
  DestroyTlas(fallback_tlas_);
}

bool RayTracingContext::EnsureBlasScratch(u64 size) {
  if (blas_scratch_ && blas_scratch_.size >= size) return true;
  device_.WaitIdle();
  device_.DestroyBuffer(blas_scratch_);
  blas_scratch_ = device_.CreateBuffer(size, kBufferUsageAccelScratch);
  return static_cast<bool>(blas_scratch_);
}

void RayTracingContext::DestroyTlas(Tlas& tlas) {
  if (tlas.handle) device_.DestroyAccelStruct(tlas.handle);
  device_.DestroyBuffer(tlas.instances);
  device_.DestroyBuffer(tlas.scratch);
  tlas = {};
}

bool RayTracingContext::BuildBlasFromGeometries(
    const base::Vector<AccelTriangles>& geometries, Blas& out) {
  if (geometries.empty()) return false;

  // Compaction runs when the backend has the size query (vulkan; d3d12 stubs
  // it and keeps the full-size structure). Meshes upload once, so the extra
  // blocking submit happens at load time, and static BLAS memory typically
  // shrinks to roughly half.
  AccelCompactionQueryHandle query = device_.CreateCompactionQuery(1);
  BlasBuildDesc desc{.geometries = {geometries.data(), geometries.size()},
                     .allow_compaction = static_cast<bool>(query)};
  AccelSizes sizes = device_.GetBlasSizes(desc);
  if (sizes.accel_bytes == 0) {
    if (query) device_.DestroyCompactionQuery(query);
    return false;
  }

  Blas blas;
  blas.handle = device_.CreateAccelStruct(AccelStructType::kBlas, sizes.accel_bytes);
  if (!blas.handle) {
    if (query) device_.DestroyCompactionQuery(query);
    return false;
  }

  u32 alignment = device_.caps().accel_scratch_alignment;
  if (!EnsureBlasScratch(sizes.scratch_bytes + alignment)) {
    device_.DestroyAccelStruct(blas.handle);
    if (query) device_.DestroyCompactionQuery(query);
    return false;
  }
  u64 scratch_offset = AlignUp(blas_scratch_.address, alignment) - blas_scratch_.address;

  device_.ImmediateSubmit([&](CommandList& cmd) {
    cmd.BuildBlas(blas.handle, desc, blas_scratch_, scratch_offset);
    if (query) cmd.QueryCompactedSizes(query, &blas.handle, 1);
  });
  if (query) {
    // ImmediateSubmit waited the fence, so the size is ready to read.
    u64 compacted = 0;
    if (device_.GetCompactedSizes(query, &compacted, 1) && compacted > 0 &&
        compacted < sizes.accel_bytes) {
      if (AccelStructHandle lean =
              device_.CreateAccelStruct(AccelStructType::kBlas, compacted)) {
        device_.ImmediateSubmit(
            [&](CommandList& cmd) { cmd.CopyAccelStruct(lean, blas.handle, /*compact=*/true); });
        device_.DestroyAccelStruct(blas.handle);
        blas.handle = lean;
        compacted_saved_bytes_ += sizes.accel_bytes - compacted;
      }
    }
    device_.DestroyCompactionQuery(query);
  }
  blas.address = device_.accel_address(blas.handle);
  out = blas;
  return true;
}

bool RayTracingContext::BuildBlas(u64 mesh_key, const GpuMesh& mesh) {
  if (blas_.contains(mesh_key)) return true;
  if (mesh.vertex_count == 0 || mesh.index_count == 0) return false;
  if (mesh.vertices.address == 0 || mesh.indices.address == 0) return false;

  base::Vector<AccelTriangles> geometries = BlasGeometries(mesh);
  Blas blas;
  if (!BuildBlasFromGeometries(geometries, blas)) return false;
  blas_.emplace(mesh_key, blas);
  return true;
}

bool RayTracingContext::BuildApproxBlas(u64 mesh_key,
                                        const base::Vector<AccelTriangles>& geometries) {
  if (approx_blas_.contains(mesh_key)) return true;
  Blas blas;
  if (!BuildBlasFromGeometries(geometries, blas)) return false;
  approx_blas_.emplace(mesh_key, blas);
  return true;
}

void RayTracingContext::RemoveBlas(u64 mesh_key) {
  Blas* blas = blas_.find(mesh_key);
  if (!blas) return;
  if (blas->handle) device_.DestroyAccelStruct(blas->handle);
  blas_.erase(mesh_key);
  slot_tracker_.InvalidateBuilds();  // TLAS slots built before this now stale
}

void RayTracingContext::RemoveApproxBlas(u64 mesh_key) {
  Blas* blas = approx_blas_.find(mesh_key);
  if (!blas) return;
  if (blas->handle) device_.DestroyAccelStruct(blas->handle);
  approx_blas_.erase(mesh_key);
  slot_tracker_.InvalidateBuilds();
}

bool RayTracingContext::BuildLodBlas(u64 mesh_key, u32 lod,
                                     const base::Vector<AccelTriangles>& geometries) {
  if (lod == 0) return false;
  base::Vector<Blas>* lods = lod_blas_.find(mesh_key);
  if (!lods) {
    lod_blas_.emplace(mesh_key, base::Vector<Blas>{});
    lods = lod_blas_.find(mesh_key);
  }
  if (lods->size() < lod) lods->resize(lod);
  if ((*lods)[lod - 1].handle) return true;  // already built
  Blas blas;
  if (!BuildBlasFromGeometries(geometries, blas)) return false;
  (*lods)[lod - 1] = blas;
  return true;
}

bool RayTracingContext::HasLodBlas(u64 mesh_key, u32 lod) const {
  if (lod == 0) return false;
  const base::Vector<Blas>* lods = lod_blas_.find(mesh_key);
  return lods && lods->size() >= lod && (*lods)[lod - 1].handle;
}

void RayTracingContext::RemoveLodBlas(u64 mesh_key) {
  base::Vector<Blas>* lods = lod_blas_.find(mesh_key);
  if (!lods) return;
  for (Blas& blas : *lods)
    if (blas.handle) device_.DestroyAccelStruct(blas.handle);
  lod_blas_.erase(mesh_key);
  slot_tracker_.InvalidateBuilds();
}

bool RayTracingContext::EnsureTlasCapacity(Tlas& tlas, u32 instance_count) {
  if (tlas.handle && tlas.capacity >= instance_count) return true;

  u32 capacity = 64;
  while (capacity < instance_count) capacity *= 2;

  // Build the replacement transactionally. Allocation failure preserves the
  // old resources; the slot tracker decides whether they are safe to read.
  Tlas replacement;
  replacement.instances = device_.CreateBuffer(capacity * sizeof(TlasInstance),
                                                kBufferUsageAccelBuildInput, true);
  if (!replacement.instances.mapped) {
    DestroyTlas(replacement);
    return false;
  }

  AccelSizes sizes = device_.GetTlasSizes(capacity);
  replacement.handle = device_.CreateAccelStruct(AccelStructType::kTlas, sizes.accel_bytes);
  u32 alignment = device_.caps().accel_scratch_alignment;
  replacement.scratch =
      device_.CreateBuffer(sizes.scratch_bytes + alignment, kBufferUsageAccelScratch);
  if (!replacement.handle || !replacement.scratch) {
    DestroyTlas(replacement);
    return false;
  }
  replacement.capacity = capacity;

  if (tlas.handle) device_.WaitIdle();
  DestroyTlas(tlas);
  tlas = replacement;
  return true;
}

bool RayTracingContext::ReserveTlas(u32 slot, u32 instance_count) {
  // Reserve for the upper bound (some instances may lack a BLAS and drop out in
  // BuildTlas, but never more than this); a stall/realloc here is safe.
  if (EnsureTlasCapacity(tlas_[slot], std::max(instance_count, 1u))) return true;
  slot_tracker_.Invalidate(slot);
  RX_ERROR("tlas slot {} allocation failed; using the empty fallback", slot);
  return false;
}

void RayTracingContext::BuildTlas(CommandList& cmd, u32 slot, u32 frame_index,
                                  const base::Vector<Instance>& instances) {
  Tlas& tlas = tlas_[slot];

  base::Vector<TlasInstance> gpu_instances;
  gpu_instances.reserve(instances.size());
  for (const Instance& instance : instances) {
    const Blas* blas = nullptr;
    if (instance.approx) {
      blas = approx_blas_.find(instance.mesh_key);
    } else if (instance.lod > 0) {
      const base::Vector<Blas>* lods = lod_blas_.find(instance.mesh_key);
      if (lods && lods->size() >= instance.lod && (*lods)[instance.lod - 1].handle)
        blas = &(*lods)[instance.lod - 1];
    } else {
      blas = blas_.find(instance.mesh_key);
    }
    if (!blas || !blas->handle) continue;
    TlasInstance gpu{};
    ToInstanceTransform(instance.transform, gpu.transform);
    gpu.custom_index = instance.custom_index & 0xffffffu;
    gpu.mask = instance.mask;
    gpu.flags = kTlasInstanceTriangleCullDisable;
    gpu.blas_address = blas->address;
    gpu_instances.push_back(gpu);
  }

  u32 count = static_cast<u32>(gpu_instances.size());
  if (!tlas.handle || !tlas.instances.mapped || !tlas.scratch ||
      tlas.capacity < std::max(count, 1u)) {
    slot_tracker_.Invalidate(slot);
    RX_ERROR("tlas slot {} was not reserved; using the empty fallback", slot);
    return;
  }
  if (count > 0) {
    std::memcpy(tlas.instances.mapped, gpu_instances.data(), count * sizeof(TlasInstance));
  }

  cmd.BuildTlas(tlas.handle, tlas.instances, count, tlas.scratch);
  cmd.MemoryBarrier(BarrierScope::kAccelBuildWrite, BarrierScope::kAccelRead);
  // The slot now holds a valid build against the current BLAS set; consumers may
  // read it next frame (async) or this frame (sync). Marked after the successful
  // record so the allocation-failure early-out above leaves the slot invalid.
  slot_tracker_.MarkBuilt(slot, frame_index);
}

}  // namespace rx::render
