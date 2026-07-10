#ifndef RX_ASSET_PRIMITIVES_H_
#define RX_ASSET_PRIMITIVES_H_

#include "asset/mesh.h"
#include "asset/skeleton.h"
#include "core/export.h"

namespace rx::asset {

// Procedural test shapes for bringup and unit tests.
RX_ASSET_EXPORT Mesh MakeCube(f32 half_extent, AssetId id);

// An axis-aligned box with per-axis half extents (a cube is the uniform case),
// for wall slabs and rooms. The submesh list is left EMPTY: callers append
// their own (the Cornell scene builds multi-material boxes); a submesh-less
// mesh uploads as one full-range draw with a null material.
RX_ASSET_EXPORT Mesh MakeBox(f32 hx, f32 hy, f32 hz, AssetId id);

// A uv sphere with smooth normals, tangents and equirect uvs. One empty
// submesh is appended so the caller only has to set its material. Used by the
// material preview scene where clearcoat/sheen/anisotropy read best on a curve.
RX_ASSET_EXPORT Mesh MakeSphere(f32 radius, u32 rings, u32 segments, AssetId id);

// A uv sphere with three levels of detail (fine, medium, coarse tessellation)
// for exercising distance-based lod selection. Each lod has one empty submesh.
RX_ASSET_EXPORT Mesh MakeLodSphere(f32 radius, AssetId id);

// Appends decimated lods to a single-lod static mesh via vertex clustering
// (snap vertices to a coarse grid, collapse the triangles that fold up), so the
// distance-lod path applies to authored meshes that ship one lod. No-op for
// skinned, multi-submesh, or already-multi-lod meshes, or tiny meshes.
RX_ASSET_EXPORT void GenerateLods(Mesh* mesh);

// A blocky biped: a skeleton using the biped rig bone-name convention the
// built-in locomotion helpers expect (so the procedural locomotion drives it)
// and a skinned box-limb mesh bound to it, authored in engine space (meters,
// Y-up). For bringup of the skinning, animation and foot IK paths without game
// data.
RX_ASSET_EXPORT void MakeSkinnedBiped(AssetId mesh_id, Skeleton* out_skeleton, Mesh* out_mesh);

}  // namespace rx::asset

#endif  // RX_ASSET_PRIMITIVES_H_
