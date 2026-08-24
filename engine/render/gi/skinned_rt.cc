#include "render/gi/skinned_rt.h"

#include "asset/mesh.h"
#include "core/log.h"
#include "render/core/bindless.h"
#include "render/gi/raytracing.h"
#include "render/pipeline/material_system.h"
#include "render/rhi/device.h"
#include "shaders/skin_cs_hlsl.h"

namespace rx::render {
namespace {

struct SkinPush {
  u32 vertex_count;
  u32 skin_offset;
};

constexpr u32 kThreads = 64;  // mirrors [numthreads(64,1,1)] in skin.cs.hlsl

// skin.cs.hlsl walks both streams with hardcoded byte offsets, because a
// ByteAddressBuffer has no struct layout to inherit. Growing either type
// silently shears the deformed output, so state the coupling here.
static_assert(sizeof(asset::Vertex) == 52, "skin.cs.hlsl hardcodes the 52-byte vertex stride");
static_assert(sizeof(asset::SkinnedVertexExtra) == 8,
              "skin.cs.hlsl reads the bone indices/weights as one uint2 per vertex");

}  // namespace

bool SkinnedRayTracing::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_skin_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kByteBuffer},
                          {1, BindingType::kByteBuffer},
                          {2, BindingType::kByteBuffer},
                          {3, BindingType::kStorageBuffer}}}},
      .push_constant_size = PushSize<SkinPush>(),
      .debug_name = "skin_deform",
  });
  if (!pipeline_) {
    RX_ERROR("skinned-rt pipeline creation failed; skinned actors stay out of ray tracing");
    return false;
  }
  return true;
}

void SkinnedRayTracing::Destroy(Device& device) {
  for (Actor& actor : actors_) {
    for (Slot& slot : actor.slots)
      if (slot.posed) device.DestroyBuffer(slot.posed);
    actor = {};
  }
  actors_.clear();
  free_handles_.clear();
  recording_.clear();
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

u32 SkinnedRayTracing::Acquire() {
  if (!pipeline_) return 0;
  if (!free_handles_.empty()) {
    const u32 handle = free_handles_.back();
    free_handles_.pop_back();
    actors_[handle - 1] = {};
    return handle;
  }
  actors_.push_back({});
  return static_cast<u32>(actors_.size());
}

void SkinnedRayTracing::Release(Device& device, RayTracingContext* raytracing, u32 handle,
                                base::Vector<u32>& retire_to) {
  if (handle == 0 || handle > actors_.size()) return;
  Actor& actor = actors_[handle - 1];
  for (u32 i = 0; i < 2; ++i) {
    if (actor.slots[i].posed) device.DestroyBufferDeferred(actor.slots[i].posed);
    if (actor.slots[i].bindless != kInvalidIndex) retire_to.push_back(actor.slots[i].bindless);
    if (raytracing) raytracing->RemoveSkinnedBlasDeferred(SlotKey(handle, i));
  }
  actor = {};
  free_handles_.push_back(handle);
}

u32 SkinnedRayTracing::custom_index(u32 handle) const {
  if (handle == 0 || handle > actors_.size()) return kInvalidIndex;
  const Actor& actor = actors_[handle - 1];
  return actor.active ? actor.slots[actor.current].bindless : kInvalidIndex;
}

u64 SkinnedRayTracing::blas_key(u32 handle) const {
  if (handle == 0 || handle > actors_.size()) return 0;
  return SlotKey(handle, actors_[handle - 1].current);
}

bool SkinnedRayTracing::active(u32 handle) const {
  return handle != 0 && handle <= actors_.size() && actors_[handle - 1].active;
}

u32 SkinnedRayTracing::Prepare(Device& device, BindlessRegistry& bindless,
                               const MaterialSystem& materials, RayTracingContext& raytracing,
                               const base::UnorderedMap<u64, GpuMesh>& meshes,
                               const base::Vector<Request>& requests) {
  recording_.clear();
  for (Actor& actor : actors_) actor.active = false;
  if (!pipeline_) return 0;

  for (const Request& request : requests) {
    if (request.handle == 0 || request.handle > actors_.size()) continue;
    Actor& actor = actors_[request.handle - 1];
    // Two draws sharing one handle would deform and refit the same buffer
    // twice in one frame. One handle per skinned draw is what the header asks
    // for; honour the first request and skip the redundant work.
    if (actor.active) continue;
    const GpuMesh* mesh = meshes.find(request.mesh_key);
    // The bind-pose buffers must be readable by compute and by an AS build.
    // UploadMesh gives every mesh those usages whenever ray tracing exists, so
    // a miss here means the draw's mesh is not uploaded or is not skinned.
    // Say so: dropping it silently is exactly how a character ends up with no
    // ray-traced shadow and no clue why.
    if (!mesh || !mesh->skinned || !mesh->skinning || mesh->vertex_count == 0 ||
        mesh->indices.address == 0 || mesh->vertices.address == 0) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        RX_WARN("skinned-rt: mesh {:#x} is not an uploaded skinned mesh; the draw stays out of "
                "ray tracing", request.mesh_key);
      }
      continue;
    }
    // A mesh swap under a live handle would leave the BLAS refitting geometry
    // of the wrong size. Fail it loudly instead of tracing garbage.
    if (actor.live && actor.mesh_key != request.mesh_key) {
      RX_ERROR("skinned-rt handle {} changed mesh; release and re-acquire it", request.handle);
      continue;
    }
    actor.skin_offset = request.skin_offset;

    if (!actor.live) {
      // Both halves of the ping-pong are built up front so the alternation
      // below never has to allocate mid-flight. Failure of either rolls the
      // whole actor back: half an actor would instance a structure whose
      // partner it can never refit from.
      const u64 bytes = static_cast<u64>(mesh->vertex_count) * sizeof(asset::Vertex);
      Slot slots[2];
      bool ok = true;
      for (u32 i = 0; i < 2 && ok; ++i) {
        slots[i].posed = device.CreateBuffer(
            bytes, kBufferUsageStorage | kBufferUsageDeviceAddress |
                       kBufferUsageAccelBuildInput);
        if (!slots[i].posed) {
          RX_ERROR("skinned-rt: {} byte deformed buffer allocation failed", bytes);
          ok = false;
          break;
        }
        // One geometry per non-blend submesh over this slot's deformed buffer,
        // and the bindless geometry records in the SAME order: hit shaders
        // resolve the material from CommittedGeometryIndex through that table.
        // Only the vertex address differs between the slots, which is exactly
        // what a refit is allowed to change.
        base::Vector<AccelTriangles> geometries;
        base::Vector<BindlessRegistry::GeometryRecord> records;
        for (const GpuSubmesh& submesh : mesh->submeshes) {
          if (submesh.blend || submesh.index_count == 0) continue;
          records.push_back({submesh.index_offset, materials.bindless_material(submesh.material)});
          geometries.push_back({.vertex_address = slots[i].posed.address,
                                .vertex_stride = sizeof(asset::Vertex),
                                .vertex_count = mesh->vertex_count,
                                .vertex_format = Format::kRGB32Float,
                                .index_address =
                                    mesh->indices.address + submesh.index_offset * sizeof(u32),
                                .index_count = submesh.index_count,
                                .index_type = IndexType::kUint32,
                                .opaque = !submesh.alpha_mask});
        }
        if (geometries.empty()) {  // nothing but blended submeshes
          ok = false;
          break;
        }
        slots[i].bindless = bindless.RegisterMesh(slots[i].posed, mesh->indices, records.data(),
                                                  static_cast<u32>(records.size()));
        if (slots[i].bindless == BindlessRegistry::kInvalidIndex) {
          RX_ERROR("skinned-rt: bindless mesh table is full");
          ok = false;
          break;
        }
        // Sized here but built over a still-empty buffer; the first two Records
        // deform each slot and give it its one full build.
        if (!raytracing.ReserveSkinnedBlas(SlotKey(request.handle, i), geometries)) {
          RX_ERROR("skinned-rt: refittable blas allocation failed for handle {}", request.handle);
          ok = false;
          break;
        }
      }
      if (!ok) {
        for (u32 i = 0; i < 2; ++i) {
          if (slots[i].bindless != BindlessRegistry::kInvalidIndex)
            bindless.ReleaseMesh(slots[i].bindless);
          if (slots[i].posed) device.DestroyBuffer(slots[i].posed);
          raytracing.RemoveSkinnedBlasDeferred(SlotKey(request.handle, i));
        }
        continue;
      }
      if (mesh->morph_target_count != 0) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          RX_WARN(
              "skinned-rt: mesh has {} morph targets; the ray-traced copy is skinned only, so "
              "morph deformation will not appear in shadows or reflections",
              mesh->morph_target_count);
        }
      }
      actor.mesh_key = request.mesh_key;
      actor.vertex_count = mesh->vertex_count;
      actor.slots[0] = slots[0];
      actor.slots[1] = slots[1];
      actor.current = 1;  // flipped to 0 below, so the first frame writes slot 0
      actor.live = true;
    }
    // Alternate. Every live TLAS built on an earlier frame references the other
    // slot, so the refit recorded below cannot be rewriting a structure that a
    // queue is still traversing.
    actor.current ^= 1u;
    // Buffer handles are values, so a mesh re-upload would leave these dangling;
    // refresh them every frame rather than caching the first ones seen.
    actor.base_vertices = mesh->vertices;
    actor.skin_stream = mesh->skinning;
    actor.active = true;
    recording_.push_back(request.handle);
  }
  return static_cast<u32>(recording_.size());
}

void SkinnedRayTracing::Record(CommandList& cmd, RayTracingContext& raytracing,
                               const GpuBuffer& bone_palette) {
  if (recording_.empty() || !pipeline_ || !bone_palette) return;

  cmd.BeginDebugLabel("skin_deform");
  // Both halves of this frame's slot were last written two frames ago and read
  // one frame ago, by ray queries and by hit shading. Widest scope on both
  // sides because the two writes below differ in kind: the dispatches overwrite
  // a vertex buffer, the builds overwrite an acceleration structure.
  cmd.MemoryBarrier(BarrierScope::kAllCommands, BarrierScope::kAllCommands);
  cmd.BindPipeline(pipeline_);
  for (u32 handle : recording_) {
    const Actor& actor = actors_[handle - 1];
    cmd.BindTransient(0, {Bind::ByteBuffer(0, actor.base_vertices),
                          Bind::ByteBuffer(1, actor.skin_stream),
                          Bind::ByteBuffer(2, bone_palette),
                          Bind::StorageBuffer(3, actor.slots[actor.current].posed)});
    cmd.Push(SkinPush{actor.vertex_count, actor.skin_offset});
    cmd.Dispatch((actor.vertex_count + kThreads - 1) / kThreads, 1, 1);
  }
  // Two consumers of the same write, hence the widest destination: the builds
  // below read the posed vertices as build input, and hit shading reads them
  // through the bindless geometry table in compute AND fragment.
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kAllCommands);
  for (u32 handle : recording_) {
    const Actor& actor = actors_[handle - 1];
    raytracing.RecordSkinnedBlas(cmd, SlotKey(handle, actor.current),
                                 SlotKey(handle, actor.current ^ 1u));
  }
  // The TLAS build recorded after this reads these structures, at the build
  // stage rather than through a ray query. When that build runs on the async
  // queue the fork semaphore carries the same edge across queues, which is why
  // this pass MUST be recorded before the first async pass (see the caller).
  cmd.MemoryBarrier(BarrierScope::kAccelBuildWrite, BarrierScope::kAccelBuildWrite);
  cmd.EndDebugLabel();
}

}  // namespace rx::render
