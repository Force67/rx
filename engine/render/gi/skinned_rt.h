#ifndef RX_RENDER_SKINNED_RT_H_
#define RX_RENDER_SKINNED_RT_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/command_list.h"
#include "render/rhi/resources.h"

namespace rx::render {

class BindlessRegistry;
class Device;
class MaterialSystem;
class RayTracingContext;

// GPU skinning of skinned actors into ray-traceable geometry. A skinned mesh's
// BLAS otherwise holds its bind pose (skinning runs only in the raster vertex
// stage), so this deforms the mesh in compute into a per-actor vertex buffer
// (ordinary asset::Vertex layout) and refits a per-actor BLAS in place: every
// ray-traced effect sees the animated pose for one dispatch plus one refit.
//
// State is per ACTOR, not per mesh: each actor gets its own deformed buffer,
// bindless mesh record (instanceCustomIndex, so hit shaders read its vertices)
// and BLAS, named by an opaque handle held for the actor's lifetime.
//
// Each actor holds TWO of everything, alternating by frame, so the async TLAS
// build stays on: frame N writes slot N&1 and this frame's TLAS references it
// while earlier TLASes reference the other slot. Price: in async mode the
// ray-traced pose is one frame behind the rasterized one. The alternation runs
// in both modes; SelectTlasSlots can flip slots frame to frame, so nothing may
// key off a fixed slot.
//
// Morph targets are NOT applied: a morphed actor registers anyway (skinning is
// what moves the silhouette) but warns once, rather than quietly ray tracing a
// face that does not match the rasterized one.
class SkinnedRayTracing {
 public:
  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  // Handle lifetime. Acquire returns 0 only when the pass is unavailable, and 0
  // is never a valid handle, so a game can store the result unconditionally.
  u32 Acquire();
  // Retires the actor's deformed buffers and structures (deferred, so an
  // in-flight frame can still be reading them) and appends its bindless mesh
  // indices to `retire_to` for the caller to retire on ITS ring. Handing them
  // back rather than releasing them here is the point: a submitted TLAS
  // instance still carries the index, and a slot returned straight to the free
  // list would be reused under that frame. Safe for a handle that never drew.
  void Release(Device& device, RayTracingContext* raytracing, u32 handle,
               base::Vector<u32>& retire_to);
  void InvalidateMesh(Device& device, RayTracingContext* raytracing, u64 mesh_key,
                       base::Vector<u32>& retire_to);
  void BeginFrame();

  // One skinned draw to deform this frame.
  struct Request {
    u32 handle = 0;      // from Acquire
    u64 mesh_key = 0;    // the renderer's mesh key (already salted)
    u32 skin_offset = 0; // first bone of this draw in the frame bone palette
  };

  // CPU frame-build phase: allocates whatever the requested actors still need
  // (deformed buffer, bindless record, BLAS + refit scratch) and drops requests
  // it cannot serve. Allocation lives here and not in Record for the same
  // reason RayTracingContext::ReserveTlas does: a mid-record allocation can
  // stall or free a resource the command list still references. Returns the
  // number of actors that will actually be recorded.
  u32 Prepare(Device& device, BindlessRegistry& bindless, const MaterialSystem& materials,
              RayTracingContext& raytracing,
              const base::UnorderedMap<u64, GpuMesh>& meshes,
              const base::Vector<Request>& requests);

  // The bindless mesh record an actor's TLAS instance must carry, so hit
  // shaders resolve to the posed vertices of the slot the same instance's
  // structure holds. kInvalidIndex when the actor was not prepared this frame.
  static constexpr u32 kInvalidIndex = 0xffffffffu;
  u32 custom_index(u32 handle) const;
  u32 previous_custom_index(u32 handle) const;
  // The RayTracingContext key of the structure this frame's TLAS instance must
  // reference (RayTracingContext::Instance::mesh_key with `skinned` set).
  u64 blas_key(u32 handle) const;
  // Whether this actor was prepared this frame and so has a BLAS to instance.
  bool active(u32 handle) const;

  // Records the frame's skinning dispatches and the BLAS builds/refits that
  // follow them, with the barriers between. MUST be recorded before the TLAS
  // build that instances these actors, on the same queue: a refit rewrites the
  // structure the TLAS references, so a cross-queue overlap would traverse a
  // half-written BLAS.
  void Record(CommandList& cmd, RayTracingContext& raytracing, const GpuBuffer& bone_palette);

 private:
  // Alternating half of an actor. The deformed buffer is per-slot too, not just
  // the structure: hit shading fetches position and normal from the buffer the
  // instance's custom_index names, so a shared buffer would pair this frame's
  // attributes with last frame's geometry. Two 21k-vertex copies are ~2 MB.
  struct Slot {
    GpuBuffer posed;  // asset::Vertex layout, deformed on the frames it is current
    u32 bindless = kInvalidIndex;
    bool valid = false;
  };

  struct Actor {
    u64 mesh_key = 0;
    u32 vertex_count = 0;
    u32 skin_offset = 0;
    Slot slots[2];
    u32 current = 0;          // the slot this frame deforms and instances
    GpuBuffer base_vertices;  // the mesh's bind-pose buffer (not owned)
    GpuBuffer skin_stream;    // the mesh's bone index/weight buffer (not owned)
    bool live = false;    // buffers + structures exist
    bool active = false;  // requested this frame
    bool allocated = false;
  };

  // RayTracingContext key for one slot. The two halves must be distinct keys in
  // its map; the shift keeps them adjacent and derivable without a side table.
  static u64 SlotKey(u32 handle, u32 slot) {
    return (static_cast<u64>(handle) << 1) | slot;
  }

  void RetireActor(Device& device, RayTracingContext* raytracing, u32 handle,
                    base::Vector<u32>& retire_to);

  // Recycles a released handle's slot. Handles are 1-based so 0 stays "none".
  base::Vector<Actor> actors_;
  base::Vector<u32> free_handles_;
  base::Vector<u32> recording_;  // handle-1 of every actor Record must touch
  PipelineHandle pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_SKINNED_RT_H_
