#ifndef RX_RENDER_RAYTRACING_H_
#define RX_RENDER_RAYTRACING_H_

#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/math.h"
#include "core/types.h"
#include "render/rhi/command_list.h"
#include "render/rhi/resources.h"

namespace rx::render {

class Device;

struct RayTracingSettings {
  bool shadows = true;
  bool reflections = false;
  bool global_illumination = false;
};

// TLAS instance mask bits. Mirrored by the RX_RAY_MASK_* defines in
// shaders/rhi_bindings.hlsli; keep the two in sync. Realtime effects
// (shadows, RTAO, reflections, fog, water, forward hits) trace with
// kRayMaskRealtime; the path-tracer family traces with kRayMaskPathTrace,
// which additionally sees no_rt fill geometry (grass-like meshes that are
// too dense for realtime rays but wanted for ground-truth light transport).
//
// kRayMaskApprox tags the opaque-approximation variant of alpha-masked
// vegetation (see the shrunk-triangle "opaque approximation" BLAS built by
// BuildApproxBlas). Realtime diffuse GI / AO / shadow rays trace
// kRayMaskRealtime|kRayMaskApprox with RAY_FLAG_CULL_NON_OPAQUE and hit the
// stand-in instead of the real (non-opaque) masked geometry; reflections and
// the path tracer never carry the approx bit, so no ray sees both variants.
enum RayMask : u8 {
  kRayMaskRealtime = 0x01,
  kRayMaskPathTrace = 0x02,
  kRayMaskApprox = 0x04,
  kRayMaskAll = 0xff,
};

// Owns acceleration structures. Only constructed when DeviceCaps::raytracing
// is true; every effect it feeds degrades to the raster path otherwise.
// BLASes build once per uploaded mesh, the TLAS rebuilds every frame from
// the visible instances. Two TLAS slots ping pong so a rebuild never races
// the frame still in flight.
class RayTracingContext {
 public:
  // Three ping-pong slots (not two): the async-TLAS path (RX_RT_ASYNC_TLAS)
  // builds slot N on the compute queue for the *next* frame while this frame's
  // consumers read the slot built last frame, so a slot is written, then read a
  // frame later, then rewritten a frame after that -- three live slots at two
  // frames in flight. The synchronous path builds and consumes one slot.
  static constexpr u32 kSlots = 3;

  struct Instance {
    u64 mesh_key = 0;
    u32 custom_index = 0;  // shader-visible instanceCustomIndex (bindless mesh record)
    u8 mask = kRayMaskAll;  // RayMask bits; rays only hit instances whose mask
                            // intersects the query's InstanceInclusionMask
    // Points the instance at this mesh's opaque-approximation BLAS (built by
    // BuildApproxBlas) instead of its regular BLAS. Used with kRayMaskApprox
    // to route realtime diffuse/AO/shadow rays to the vegetation stand-in.
    bool approx = false;
    // Selects a distance LOD's BLAS (built by BuildLodBlas) instead of the LOD0
    // structure. 0 = LOD0 (blas_), N>0 = lods_[N-1]. Mutually exclusive with
    // approx (distant LODs stay force-opaque; see BuildLodBlas).
    u32 lod = 0;
    Mat4 transform = Mat4::Identity();
  };

  static std::unique_ptr<RayTracingContext> Create(Device& device);
  ~RayTracingContext();

  RayTracingContext(const RayTracingContext&) = delete;
  RayTracingContext& operator=(const RayTracingContext&) = delete;

  void Configure(const RayTracingSettings& settings) { settings_ = settings; }
  const RayTracingSettings& settings() const { return settings_; }

  // Builds a BLAS for an uploaded mesh, keyed like the renderer's mesh map.
  // The mesh buffers must have been created with acceleration structure
  // build input usage.
  bool BuildBlas(u64 mesh_key, const GpuMesh& mesh);
  void RemoveBlas(u64 mesh_key);

  // Builds the opaque-approximation BLAS for an alpha-masked mesh: a fully
  // OPAQUE structure over the caller-provided (already centroid-shrunk)
  // vegetation geometry. Keyed like BuildBlas; the caller owns the vertex/
  // index buffers (they must outlive the BLAS and carry accel-build-input
  // usage). Instances with Instance::approx set resolve to this structure.
  bool BuildApproxBlas(u64 mesh_key, const base::Vector<AccelTriangles>& geometries);
  void RemoveApproxBlas(u64 mesh_key);
  bool HasApproxBlas(u64 mesh_key) const { return approx_blas_.contains(mesh_key); }

  // Builds (once) the BLAS for a non-zero distance LOD of an already-uploaded
  // mesh, keyed by mesh_key + lod. lod is 1-based here (lod 1 = lods_[0]); the
  // renderer builds these lazily the first time an instance selects the LOD.
  // Distant LODs are built fully OPAQUE (masked foliage stays force-opaque --
  // the opaque-approximation shrink is imperceptible past the LOD switch
  // distance), so no separate approx variant is needed for them.
  bool BuildLodBlas(u64 mesh_key, u32 lod, const base::Vector<AccelTriangles>& geometries);
  bool HasLodBlas(u64 mesh_key, u32 lod) const;
  void RemoveLodBlas(u64 mesh_key);

  // Whether a blas already exists for this mesh (BuildBlas is idempotent, but
  // callers re-registering bindless geometry need to skip already-built meshes).
  bool HasBlas(u64 mesh_key) const { return blas_.contains(mesh_key); }

  // Grows the slot's TLAS to hold at least instance_count instances. This can
  // stall (device idle) and reallocate, so it MUST be called during the CPU
  // frame-build phase, never while a command list is recording. Doing the
  // growth here keeps BuildTlas allocation-free at record time.
  void ReserveTlas(u32 slot, u32 instance_count);

  // Records a full TLAS rebuild into cmd, including the barrier that makes
  // it visible to shader ray queries. Instances without a BLAS are skipped.
  // Capacity must already cover the instances (see ReserveTlas).
  void BuildTlas(CommandList& cmd, u32 slot, const base::Vector<Instance>& instances);

  AccelStructHandle tlas(u32 slot) const { return tlas_[slot].handle; }

  // Total BLAS bytes reclaimed by compaction so far (0 on backends without
  // the compacted-size query).
  u64 compacted_saved_bytes() const { return compacted_saved_bytes_; }

 private:
  struct Blas {
    AccelStructHandle handle;
    u64 address = 0;
  };

  struct Tlas {
    AccelStructHandle handle;
    GpuBuffer instances;  // host visible TlasInstance[]
    GpuBuffer scratch;
    u32 capacity = 0;
  };

  explicit RayTracingContext(Device& device) : device_(device) {}

  // Shared build path for BuildBlas / BuildApproxBlas (create, build, compact).
  bool BuildBlasFromGeometries(const base::Vector<AccelTriangles>& geometries, Blas& out);
  bool EnsureTlasCapacity(Tlas& tlas, u32 instance_count);
  bool EnsureBlasScratch(u64 size);
  void DestroyTlas(Tlas& tlas);

  Device& device_;
  RayTracingSettings settings_;
  base::UnorderedMap<u64, Blas> blas_;
  // Opaque-approximation BLASes for alpha-masked vegetation, keyed like blas_.
  base::UnorderedMap<u64, Blas> approx_blas_;
  // Distance-LOD BLASes, keyed by mesh_key; the vector is indexed by lod-1
  // (lod 1 = [0]) and grown lazily as LODs are first selected.
  base::UnorderedMap<u64, base::Vector<Blas>> lod_blas_;
  // Reused across builds. Freeing scratch right after the fence tripped
  // lavapipe, whose build workers can outlive the signal; a persistent
  // arena avoids both that and the per-build allocation.
  GpuBuffer blas_scratch_;
  u64 compacted_saved_bytes_ = 0;
  Tlas tlas_[kSlots];
};

}  // namespace rx::render

#endif  // RX_RENDER_RAYTRACING_H_
