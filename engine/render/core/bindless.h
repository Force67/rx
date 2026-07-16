#ifndef RX_RENDER_BINDLESS_H_
#define RX_RENDER_BINDLESS_H_

#include <memory>

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/device.h"

namespace rx::render {

// GPU-side scene tables for ray hit shading: every uploaded mesh, geometry
// range, material and base color texture is reachable from a shader given a
// TLAS hit (instanceCustomIndex -> mesh record -> geometry record ->
// material record -> texture). Vertex and index data carry a dual identity:
// buffer device addresses for the SPIR-V readers (vk::RawBufferLoad) and
// bindless ByteAddressBuffer slots for DXIL, which has no BDA (see
// shaders/rt_geometry.hlsli).
class BindlessRegistry {
 public:
  static constexpr u32 kMaxTextures = 4096;
  static constexpr u32 kMaxMeshes = 16384;
  static constexpr u32 kMaxGeometries = 1u << 17;
  static constexpr u32 kMaxMaterials = 16384;
  static constexpr u32 kInvalidIndex = 0xffffffffu;
  // Geometry buffer array: two slots per mesh (vertex, index).
  static constexpr u32 kMaxGeometryBuffers = 2 * kMaxMeshes;
  // Binding number of the geometry buffer array. On d3d12 register numbers
  // derive from the binding, so it must clear the texture array's range
  // (t3 + kMaxTextures). Mirrored by RX_GEOMETRY_BUFFER_BINDING in
  // shaders/rt_geometry.hlsli.
  static constexpr u32 kGeometryBufferBinding = 4100;

  // Layouts match the StructuredBuffer declarations in ddgi_rays.cs.hlsl.
  struct MeshRecord {
    u64 vertex_address = 0;
    u64 index_address = 0;
    u32 geometry_offset = 0;
    u32 vertex_srv = 0;  // geometry-buffer-array slot of the vertex buffer
    u32 index_srv = 0;   // geometry-buffer-array slot of the index buffer
    u32 pad = 0;
  };
  struct GeometryRecord {
    u32 index_offset = 0;
    u32 material_index = 0;
  };
  struct MaterialRecord {
    f32 base_color_factor[4] = {1, 1, 1, 1};
    f32 emissive[3] = {0, 0, 0};
    u32 base_color_texture = kInvalidIndex;
    u32 flags = 0;  // bit0: alpha mask (cutout); bit1: terrain splat
    f32 alpha_cutoff = 0.5f;
    f32 roughness = 1.0f;  // scalar factors; the path tracer multiplies these by
    f32 metallic = 0.0f;   // the metallic-roughness map (.g rough, .b metal).
    u32 metallic_roughness_texture = kInvalidIndex;  // bindless index, or invalid;
                                                     // terrain reuses it for land layer 2
    u32 terrain_layer1_texture = kInvalidIndex;  // terrain land layer 1
    u32 terrain_weight_texture = kInvalidIndex;  // terrain per-cell weight/control map
    u32 pad2 = 0;  // pad to 64B: the std430 array stride rounds up to a multiple
                   // of 16 (float4 alignment), so every shader struct must match.
    // --- Skin subsurface scattering (appended; existing offsets unchanged).
    // Physical coefficients pre-mapped from artist colour/mfp via Kulla-Conty at
    // upload; only meaningful when flags has kMaterialSkin. Mirrored by the
    // shared shaders/material_record.hlsli. sigma_a = sigma_t - sigma_s.
    f32 sss_sigma_t[3] = {0, 0, 0};  // row 4: extinction (1/world-unit)
    f32 sss_anisotropy_g = 0.0f;     //        Henyey-Greenstein g
    f32 sss_sigma_s[3] = {0, 0, 0};  // row 5: scattering coefficient
    f32 sss_perfusion = 0.0f;        //        dynamic hemoglobin 0..1
    f32 sss_scatter_color[3] = {0, 0, 0};  // row 6: multiple-scatter tint
    f32 sss_ior = 1.4f;              //        boundary index of refraction
  };
  static_assert(sizeof(MaterialRecord) % 16 == 0, "bindless material stride must be 16-aligned");
  static_assert(sizeof(MaterialRecord) == 112, "bindless material record must match shaders/material_record.hlsli");
  static constexpr u32 kMaterialAlphaMask = 1u << 0;
  static constexpr u32 kMaterialTerrain = 1u << 1;
  // Skin subsurface scattering. Bit value matches MaterialSystem::kFlagSkin and
  // RX_MATERIAL_FLAG_SKIN in shaders/material_record.hlsli so the raster and RT
  // flag namespaces agree.
  static constexpr u32 kMaterialSkin = 1u << 6;

  static std::unique_ptr<BindlessRegistry> Create(Device& device);
  ~BindlessRegistry();

  BindlessRegistry(const BindlessRegistry&) = delete;
  BindlessRegistry& operator=(const BindlessRegistry&) = delete;

  // All return kInvalidIndex when the respective table is full.
  u32 RegisterTexture(TextureView view);
  u32 RegisterMaterial(const MaterialRecord& record);
  // Texture streaming support. ReleaseTexture returns a slot to a free list
  // that RegisterTexture reuses. The caller owns the timing: release only
  // after every in-flight frame that could read the slot has drained AND no
  // material record still points at it (the material system's retire ring
  // guarantees both). RewriteTextureIndex repoints a material record's
  // texture references (all four fields checked) from one slot to another;
  // the table is host visible, so pending frames read either index - both
  // stay valid images until the old slot is released.
  void ReleaseTexture(u32 index);
  void RewriteTextureIndex(u32 material_index, u32 old_texture, u32 new_texture);
  // Geometry records must follow the blas geometry order (non-blend
  // submeshes in submesh order). Returns the instanceCustomIndex.
  // The buffers must have been created with kBufferUsageDeviceAddress.
  u32 RegisterMesh(const GpuBuffer& vertices, const GpuBuffer& indices,
                   const GeometryRecord* geometries, u32 geometry_count);

  BindingLayoutHandle set_layout() const { return set_layout_; }
  BindingSetHandle set() const { return set_; }

 private:
  explicit BindlessRegistry(Device& device) : device_(device) {}
  bool Initialize();

  Device& device_;
  BindingLayoutHandle set_layout_;
  BindingSetHandle set_;

  GpuBuffer mesh_table_;      // host visible MeshRecord[]
  GpuBuffer geometry_table_;  // host visible GeometryRecord[]
  GpuBuffer material_table_;  // host visible MaterialRecord[]
  u32 mesh_count_ = 0;
  u32 geometry_count_ = 0;
  u32 material_count_ = 0;
  u32 texture_count_ = 0;
  base::Vector<u32> free_textures_;  // released slots, reused by RegisterTexture
};

}  // namespace rx::render

#endif  // RX_RENDER_BINDLESS_H_
