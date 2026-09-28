// Dual-path mesh geometry readers for ray-query hit shading.
//
// The scene tables hand a hit shader per-mesh vertex/index data two ways:
// buffer device addresses (SPIR-V, vk::RawBufferLoad) and slots into a
// bindless ByteAddressBuffer array (DXIL, which has no BDA). Structs mirror
// render::BindlessRegistry (core/bindless.h); the array binding number
// mirrors BindlessRegistry::kGeometryBufferBinding and must clear the
// bindless texture range (t3 + kMaxTextures) because d3d12 registers derive
// from binding numbers.
//
// Usage: #define RX_GEOMETRY_SPACE spaceN (the bindless set's space in the
// including shader) before including, then read through the Rx* helpers.

#ifndef RX_RT_GEOMETRY_HLSLI_
#define RX_RT_GEOMETRY_HLSLI_

struct MeshRecord {
  uint64_t vertex_address;
  uint64_t index_address;
  uint geometry_offset;
  uint vertex_srv;  // geometry-buffer-array slot of the vertex buffer
  uint index_srv;   // geometry-buffer-array slot of the index buffer
  uint pad;
};

struct GeometryRecord {
  uint index_offset;
  uint material_index;
};

// asset::Vertex layout: position f3 @0, normal f3 @12, tangent f4 @24,
// uv f2 @40, color rgba8 @48; stride 52.
static const uint kRxVertexStride = 52;
static const uint kRxNormalOffset = 12;
static const uint kRxUvOffset = 40;

#ifndef __spirv__
// 2 * BindlessRegistry::kMaxMeshes; the count and base register must match
// the layout in bindless.cc for root-signature validation.
ByteAddressBuffer rx_geometry_buffers[32768] : register(t4100, RX_GEOMETRY_SPACE);
#endif

// The bindless texture array shares the geometry space, so on DXIL it must be
// bounded (BindlessRegistry::kMaxTextures) or its unbounded register range
// t3.. would overlap rx_geometry_buffers at t4100. SPIR-V keeps the runtime
// array the vulkan variable-count descriptor set expects. Including shaders
// declare: Texture2D bindless_textures[RX_BINDLESS_TEXTURE_COUNT].
#ifdef __spirv__
#define RX_BINDLESS_TEXTURE_COUNT
#else
#define RX_BINDLESS_TEXTURE_COUNT 4096
#endif

// Three u32 indices of `primitive` starting at geometry.index_offset.
uint3 RxLoadTriangle(MeshRecord mesh, uint first_index) {
#ifdef __spirv__
  uint64_t base = mesh.index_address + first_index * 4;
  return uint3(vk::RawBufferLoad<uint>(base), vk::RawBufferLoad<uint>(base + 4),
               vk::RawBufferLoad<uint>(base + 8));
#else
  return rx_geometry_buffers[NonUniformResourceIndex(mesh.index_srv)].Load3(first_index * 4);
#endif
}

float3 RxLoadPosition(MeshRecord mesh, uint vertex_index) {
#ifdef __spirv__
  return vk::RawBufferLoad<float3>(mesh.vertex_address + vertex_index * kRxVertexStride, 4);
#else
  return asfloat(rx_geometry_buffers[NonUniformResourceIndex(mesh.vertex_srv)]
                     .Load3(vertex_index * kRxVertexStride));
#endif
}

float3 RxLoadNormal(MeshRecord mesh, uint vertex_index) {
#ifdef __spirv__
  return vk::RawBufferLoad<float3>(
      mesh.vertex_address + vertex_index * kRxVertexStride + kRxNormalOffset, 4);
#else
  return asfloat(rx_geometry_buffers[NonUniformResourceIndex(mesh.vertex_srv)]
                     .Load3(vertex_index * kRxVertexStride + kRxNormalOffset));
#endif
}

float2 RxLoadUv(MeshRecord mesh, uint vertex_index) {
#ifdef __spirv__
  return vk::RawBufferLoad<float2>(
      mesh.vertex_address + vertex_index * kRxVertexStride + kRxUvOffset, 4);
#else
  return asfloat(rx_geometry_buffers[NonUniformResourceIndex(mesh.vertex_srv)]
                     .Load2(vertex_index * kRxVertexStride + kRxUvOffset));
#endif
}

#endif  // RX_RT_GEOMETRY_HLSLI_
