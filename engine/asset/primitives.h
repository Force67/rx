#ifndef RX_ASSET_PRIMITIVES_H_
#define RX_ASSET_PRIMITIVES_H_

#include "asset/mesh.h"
#include "asset/skeleton.h"

namespace rx::asset {

// Procedural test shapes for bringup and unit tests.
Mesh MakeCube(f32 half_extent, AssetId id);

// An axis-aligned box with per-axis half extents (a cube is the uniform case),
// for wall slabs and rooms. One empty submesh so the caller sets the material.
Mesh MakeBox(f32 hx, f32 hy, f32 hz, AssetId id);

// A uv sphere with smooth normals, tangents and equirect uvs. One empty
// submesh is appended so the caller only has to set its material. Used by the
// material preview scene where clearcoat/sheen/anisotropy read best on a curve.
Mesh MakeSphere(f32 radius, u32 rings, u32 segments, AssetId id);

// A uv sphere with three levels of detail (fine, medium, coarse tessellation)
// for exercising distance-based lod selection. Each lod has one empty submesh.
Mesh MakeLodSphere(f32 radius, AssetId id);

// Appends decimated lods to a single-lod static mesh via vertex clustering
// (snap vertices to a coarse grid, collapse the triangles that fold up), so the
// distance-lod path applies to authored meshes that ship one lod. No-op for
// skinned, multi-submesh, or already-multi-lod meshes, or tiny meshes.
void GenerateLods(Mesh* mesh);

// A blocky biped: a skeleton with the standard Skyrim bone names (so the
// procedural locomotion drives it) and a skinned box-limb mesh bound to it,
// authored in engine space (meters, Y-up). For bringup of the skinning,
// animation and foot IK paths without game data.
void MakeSkinnedBiped(AssetId mesh_id, Skeleton* out_skeleton, Mesh* out_mesh);

}  // namespace rx::asset

#endif  // RX_ASSET_PRIMITIVES_H_
