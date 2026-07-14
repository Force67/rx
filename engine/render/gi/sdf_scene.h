#ifndef RX_RENDER_GI_SDF_SCENE_H_
#define RX_RENDER_GI_SDF_SCENE_H_

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/job_system.h"
#include "core/types.h"
#include "render/rhi/device.h"
#include "render/rhi/resources.h"

namespace rx::render {

// Per-unique-mesh signed distance fields, generated CPU-side at mesh upload and
// stored as flat float storage buffers (local space, signed). The global SDF
// clipmap (SdfClipmap) min-blends these into camera-following volumes; S2 will
// sphere-trace that clipmap in place of a TLAS on hardware without ray query.
//
// Everything here is gated by RX_SDF upstream: the renderer only constructs an
// SdfScene when the software-trace path is enabled, so with RX_SDF unset no SDF
// is ever generated and nothing is allocated.
//
// Design notes / limitations (S1 prototype):
//  - Distance is exact point-triangle distance accelerated by a uniform triangle
//    grid; sign is a 3-axis ray-parity majority vote. Open / thin meshes leak
//    (the same failure class as Lumen's mesh SDFs) — documented, not fought.
//  - The SDF is stored as one signed float per voxel in a StructuredBuffer (not
//    a 3D texture): the RHI's CopyBufferToTexture is 2D-only, so a 3D upload
//    would need a compute copy per mesh; a flat buffer sampled with manual
//    trilinear in the compose shader is simpler and dodges the R16Float-3D
//    storage-format question. f16 packing is a future memory win (noted).
//  - Per-mesh average albedo / emissive come from material factors only (texture
//    averaging is out of scope).
class SdfScene {
 public:
  // One mesh's CPU geometry + averaged surface colour, handed in at upload.
  struct MeshInput {
    const f32* positions = nullptr;  // interleaved vertex data, xyz first
    u32 position_stride = 0;         // bytes between consecutive positions
    u32 vertex_count = 0;
    const u32* indices = nullptr;
    u32 index_count = 0;  // triangle list; 0 = non-indexed (vertex_count/3 tris)
    f32 albedo[3] = {0.6f, 0.6f, 0.65f};
    f32 emissive[3] = {0, 0, 0};
  };

  struct MeshSdf {
    GpuBuffer sdf;         // res.x*res.y*res.z signed floats, local units
    f32 box_min[3] = {};   // local-space min corner of the volume
    f32 voxel = 0;         // local-space voxel size (cubic)
    u32 res[3] = {};       // per-axis voxel count (16..64)
    f32 albedo[3] = {0.6f, 0.6f, 0.65f};
    f32 emissive[3] = {0, 0, 0};
    f32 gen_ms = 0;        // generation time for this mesh
  };

  explicit SdfScene(Device& device) : device_(device) {}
  ~SdfScene();

  SdfScene(const SdfScene&) = delete;
  SdfScene& operator=(const SdfScene&) = delete;

  // Generates + uploads the SDF for one mesh, keyed like BLASes (u64 mesh key).
  // Idempotent per key; returns false if the mesh has no triangles.
  bool RegisterMesh(u64 mesh_key, const MeshInput& input);
  const MeshSdf* Find(u64 mesh_key) const { return meshes_.find(mesh_key); }

  u32 mesh_count() const { return static_cast<u32>(meshes_.size()); }
  u64 total_bytes() const { return total_bytes_; }
  f32 total_gen_ms() const { return total_gen_ms_; }
  f32 last_gen_ms() const { return last_gen_ms_; }

 private:
  Device& device_;
  JobSystem jobs_;
  base::UnorderedMap<u64, MeshSdf> meshes_;
  u64 total_bytes_ = 0;
  f32 total_gen_ms_ = 0;
  f32 last_gen_ms_ = 0;
};

}  // namespace rx::render

#endif  // RX_RENDER_GI_SDF_SCENE_H_
