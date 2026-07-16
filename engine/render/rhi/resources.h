#ifndef RX_RENDER_RHI_RESOURCES_H_
#define RX_RENDER_RHI_RESOURCES_H_

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/types.h"

namespace rx::render {

using BufferHandle = RhiHandle<struct BufferTag>;
using TextureHandle = RhiHandle<struct TextureTag>;

// Value-type views of GPU resources. The handle points at a backend-owned
// record; everything else is mirrored here so hot paths never call into the
// backend for metadata. Copies alias the same GPU resource; the owner calls
// Device::DestroyBuffer/DestroyImage exactly once.
struct GpuBuffer {
  BufferHandle handle;
  u64 size = 0;
  void* mapped = nullptr;  // set for host visible buffers
  u64 address = 0;         // device address, set when created with kBufferUsageDeviceAddress

  explicit operator bool() const { return static_cast<bool>(handle); }
};

struct GpuImage {
  TextureHandle handle;
  TextureView view;  // whole-image default view
  Format format = Format::kUnknown;
  Extent2D extent{};
  u32 mip_levels = 1;
  u32 samples = 1;  // >1 = multisampled (MSAA geometry targets)

  explicit operator bool() const { return static_cast<bool>(handle); }
};

// Index range drawn with one material. Materials resolve at upload time so
// the draw loop is a plain array walk.
struct GpuSubmesh {
  u32 index_offset = 0;
  u32 index_count = 0;
  u64 material = 0;  // material asset hash, 0 = default material
  bool blend = false;  // alpha blended: drawn sorted after opaque
  bool water = false;  // routed to the water pipeline
  bool alpha_mask = false;  // cutout: non-opaque blas geometry for ray alpha tests
  bool effect = false;  // unlit emissive vfx (torch flames, glow planes, mist)
  bool effect_additive = false;  // effect drawn with the additive blend pipeline
  // lod 0 meshlet range in GpuMesh's meshlet buffers, for the mesh-shader path.
  u32 meshlet_offset = 0;
  u32 meshlet_count = 0;  // 0 = no meshlets built (skinned / mesh shaders off)
};

// An extra level of detail (lods 1+) inside the mesh's concatenated vertex and
// index buffers. The submesh index offsets are absolute in the shared index
// buffer; vertex_offset rebases the indices onto this lod's vertices. Lod 0 is
// GpuMesh::submeshes (vertex_offset 0).
struct GpuLod {
  u32 vertex_offset = 0;
  base::Vector<GpuSubmesh> submeshes;
};

struct GpuMesh {
  GpuBuffer vertices;  // all lods concatenated
  GpuBuffer indices;   // all lods concatenated
  GpuBuffer skinning;      // per-vertex bone indices/weights, set when skinned
  // Morph target deltas, read by device address in the mesh vertex shaders:
  // one 36-byte {position, normal, tangent} float3 triple per lod 0 vertex,
  // targets concatenated ([target][vertex]). Empty for unmorphed meshes.
  GpuBuffer morph_deltas;
  u32 morph_target_count = 0;
  u32 index_count = 0;     // lod 0 index count (shadow / overdraw / blas use lod 0)
  u32 vertex_count = 0;    // lod 0 vertex count
  bool all_blend = false;  // pure transparency (water): stays out of the tlas
  bool no_rt = false;      // grass-like fill geometry, excluded from the tlas
  bool terrain_lod = false;  // distant terrain proxy, sunk inside the streamed rect
  bool skinned = false;    // drawn with the skinned pipeline against a bone palette
  u32 bindless_index = 0;  // mesh record in the bindless registry
  f32 bounds_center[3] = {0, 0, 0};  // model-space bounding sphere, for gpu culling
  f32 bounds_radius = 0;             // 0 = unknown, treated as never-culled
  // Planar water bounds discovered at upload time. The adaptive ocean path
  // replaces only meshes that are flat in local Y; irregular rivers and
  // waterfalls keep their authored triangles.
  bool planar_water = false;
  f32 water_bounds[4] = {0, 0, 0, 0};  // local min xz, max xz
  f32 water_height = 0;
  base::Vector<GpuSubmesh> submeshes;  // lod 0
  base::Vector<GpuLod> lods;           // extra lods (1+), selected by distance

  // Opaque-approximation variant of this mesh's alpha-masked (vegetation)
  // submeshes: each masked triangle duplicated and shrunk about its centroid by
  // the baked average opacity, then flagged OPAQUE. Realtime diffuse GI / AO /
  // shadow rays hit this stand-in (RX_RAY_MASK_APPROX / kRayMaskApprox) rather
  // than the real masked geometry, which stays non-opaque for the path tracer
  // and specular reflections. Empty when the mesh has no masked submeshes.
  GpuBuffer rt_approx_vertices;
  GpuBuffer rt_approx_indices;
  u32 rt_approx_bindless = 0;  // bindless mesh record for approx hit shading
  bool rt_approx = false;      // an approx BLAS/instance exists for this mesh

  // Distance-LOD ray-tracing geometry (RX_RT_LOD_NEAR), parallel to `lods`:
  // entry [lod-1] describes LOD N's opaque RT stand-in. The rebased index
  // buffer and bindless record are prepared eagerly at upload (cheap, and the
  // CPU geometry is only in hand there); its `indices` are rebased to index the
  // shared LOD0-onward vertex buffer directly, so hit shading needs no per-LOD
  // vertex offset. The BLAS itself (the expensive blocking build) is created
  // lazily by the renderer the first time an instance selects the LOD.
  struct LodRt {
    GpuBuffer indices;      // absolute-indexed opaque geometry for this LOD
    u32 vertex_count = 0;   // maxVertex bound for the BLAS (shared vertex count)
    struct Geom {
      u32 index_offset = 0;  // first index (u32 elements) into `indices`
      u32 index_count = 0;
    };
    base::Vector<Geom> geoms;   // one per opaque submesh, BLAS-geometry order
    u32 bindless = 0xffffffffu;  // bindless mesh record, kInvalidIndex until set
    bool blas_built = false;     // BLAS built lazily on first RT selection
  };
  base::Vector<LodRt> lod_rt;

  // Mesh-shader path: lod 0 split into meshlets, read in the mesh shader via
  // buffer device address, so the GpuBuffer::address fields are what the push
  // constants carry. Empty when mesh shaders are unavailable or the mesh is
  // skinned. meshlets/meshlet_vertices/meshlet_triangles mirror MeshletPass.
  GpuBuffer meshlets;
  GpuBuffer meshlet_vertices;
  GpuBuffer meshlet_triangles;
  bool has_meshlets = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_RHI_RESOURCES_H_
