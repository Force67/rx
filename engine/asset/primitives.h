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

// Blockout shapes for text-authored scenes, alongside the box and sphere above.
// All of them carry smooth normals, tangents along +u and uvs covering the 0..1
// square exactly once (so a procedural texture tiles by its own scale, not by
// the mesh), and each appends one empty submesh for the caller to fill.
//
// The surfaces of revolution wind counter-clockwise seen from outside, like
// MakeBox; MakeSphere predates that and winds the other way, which is invisible
// only because every raster pipeline runs with culling off.

// A flat quad in the xz plane at y = 0, facing +Y. One face, not a slab: a
// backdrop or ground plane seen from behind is invisible under a culling
// pipeline even though today's do not cull.
RX_ASSET_EXPORT Mesh MakePlane(f32 hx, f32 hz, AssetId id);

// A capped cylinder along Y, `half_height` from the origin to each cap. The
// side's v runs bottom to top; the caps get their own vertices so the rim stays
// a hard edge.
RX_ASSET_EXPORT Mesh MakeCylinder(f32 radius, f32 half_height, u32 segments, AssetId id);

// A cone along Y: base of `radius` at -half_height, apex at +half_height. The
// apex ring is duplicated per segment so each column keeps its own normal
// instead of averaging to straight up.
RX_ASSET_EXPORT Mesh MakeCone(f32 radius, f32 half_height, u32 segments, AssetId id);

// A torus around the Y axis. `major_radius` is the ring, `minor_radius` the
// tube; u runs around the ring, v around the tube.
RX_ASSET_EXPORT Mesh MakeTorus(f32 major_radius, f32 minor_radius, u32 rings, u32 segments,
                               AssetId id);

// A capsule along Y: a cylinder of `half_height` capped by two hemispheres of
// `radius` (so it stands 2 * (half_height + radius) tall). v is distributed by
// profile arc length, so the texel density does not pinch at the caps.
RX_ASSET_EXPORT Mesh MakeCapsule(f32 radius, f32 half_height, u32 rings, u32 segments,
                                 AssetId id);

// Appends decimated lods to a single-lod static mesh via vertex clustering
// (snap vertices to a coarse grid, collapse the triangles that fold up), so the
// distance-lod path applies to authored meshes that ship one lod. Multi-submesh
// meshes keep their materials: each submesh is clustered on its own and the
// output submesh table matches the input entry for entry. No-op for skinned or
// already-multi-lod meshes, for meshes under a few thousand indices, and for
// meshes the clustering fails to shrink (loose cards rather than a surface).
RX_ASSET_EXPORT void GenerateLods(Mesh* mesh);

// A blocky biped: a skeleton using the biped rig bone-name convention the
// built-in locomotion helpers expect (so the procedural locomotion drives it)
// and a skinned box-limb mesh bound to it, authored in engine space (meters,
// Y-up). For bringup of the skinning, animation and foot IK paths without game
// data.
RX_ASSET_EXPORT void MakeSkinnedBiped(AssetId mesh_id, Skeleton* out_skeleton, Mesh* out_mesh);

}  // namespace rx::asset

#endif  // RX_ASSET_PRIMITIVES_H_
