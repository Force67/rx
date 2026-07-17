#ifndef RX_RUNTIME_PLACEMENT_DEMO_ASSETS_H_
#define RX_RUNTIME_PLACEMENT_DEMO_ASSETS_H_

#include "asset/mesh.h"

namespace rx {

// Hand-built low-poly nature meshes for the GPU procedural placement demo:
// the "authored assets" a scatter pass instances across the terrain. Each
// function returns a complete single-lod mesh with its bounds filled and one
// submesh per material region, so the caller only supplies material ids. All
// meshes have their origin at the ground contact point and grow +Y up, so a
// placement transform can translate to the sampled ground height and spin
// freely around Y. Every returned mesh stays under ~400 triangles.
//
// Materials are passed by AssetId hash (u64) to match the demo call sites,
// which pass their Material::id.hash; the mesh wraps them back into AssetId
// for each Submesh.

// Tapered trunk + 2-3 stacked cones. Trunk and canopy are separate submeshes.
asset::Mesh MakePineTree(f32 height, u64 trunk_material, u64 canopy_material, asset::AssetId id);

// Trunk + a cluster of overlapping squashed spheres for the canopy. Trunk and
// canopy are separate submeshes.
asset::Mesh MakeBroadleafTree(f32 height, u64 trunk_material, u64 canopy_material, asset::AssetId id);

// A low clump of overlapping squashed spheres, single material.
asset::Mesh MakeBush(f32 radius, u64 material, asset::AssetId id);

// A uv-sphere lumped by deterministic radial noise (seeded) and flattened on Y
// so it rests on the ground. Face-averaged normals, single material.
asset::Mesh MakeRock(f32 radius, u32 seed, u64 material, asset::AssetId id);

// A handful of crossed, slightly leaning blade quads, each duplicated with
// flipped winding so it reads from both sides regardless of material. Kept out
// of the ray tracing acceleration structures. Single material.
asset::Mesh MakeGrassTuft(f32 height, u64 material, asset::AssetId id);

// A bare trunk with a few tapered branch stubs at angles, single material.
asset::Mesh MakeDeadTree(f32 height, u64 material, asset::AssetId id);

}  // namespace rx

#endif  // RX_RUNTIME_PLACEMENT_DEMO_ASSETS_H_
