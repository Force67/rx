#ifndef RX_ASSET_MESH_H_
#define RX_ASSET_MESH_H_

#include <base/containers/vector.h>

#include "asset/asset_id.h"
#include "asset/skeleton.h"
#include "core/types.h"

namespace rx::asset {

// Engine native mesh. Interleaved vertex streams as the source formats give
// no benefit from keeping their layout, GPU friendly index buffer, submeshes
// per material. NIF geometry is converted into this at load time.
struct Vertex {
  f32 position[3];
  f32 normal[3];
  f32 tangent[4];
  f32 uv[2];
  u32 color = 0xffffffff;
};

struct SkinnedVertexExtra {
  u8 bone_indices[4] = {};
  u8 bone_weights[4] = {};
};

struct Submesh {
  u32 index_offset = 0;
  u32 index_count = 0;
  AssetId material;
};

struct MeshLod {
  base::Vector<Vertex> vertices;
  base::Vector<SkinnedVertexExtra> skinning;
  base::Vector<u32> indices;
  base::Vector<Submesh> submeshes;
};

// A particle emitter parsed from a NIF particle system (fires, smoke, mist),
// in the same mesh-local space as the vertices. The renderer runs a small cpu
// simulation per placed instance and draws camera-facing billboards; an
// instance's transform maps positions/velocities into engine world space.
struct ParticleEmitter {
  f32 position[3] = {0, 0, 0};  // emit volume center
  f32 velocity[3] = {0, 0, 0};  // mean birth velocity, units/sec
  f32 extent[3] = {0, 0, 0};    // emit volume half extents (axis aligned)
  f32 gravity[3] = {0, 0, 0};   // acceleration, units/sec^2
  f32 spread = 0;               // birth direction cone half angle, radians
  f32 speed_variation = 0;      // +- units/sec around |velocity|
  f32 rate = 12;                // births/sec
  f32 life = 1;                 // seconds
  f32 life_variation = 0;
  f32 size = 8;                 // billboard radius at birth
  f32 color[4] = {1, 1, 1, 1};  // additive: radiance; alpha: tint + opacity
  u32 max_particles = 64;
  bool additive = false;  // additive blend (fire/glow) vs lit alpha (smoke)
  // Effect texture bound on the billboards. At parse time this is the texture
  // asset hash; the renderer rewrites it to a bindless index at upload. 0 = the
  // procedural (untextured) billboard look.
  u64 texture = 0;
  // Flipbook atlas grid from the NiPSysData subtexture offsets: the billboard
  // animates through `subtex_frames` cells of a cols x rows grid over the
  // particle's life. 1 = the whole texture (no flipbook).
  u32 subtex_frames = 1;
  u32 subtex_cols = 1;
  u32 subtex_rows = 1;
  // BSPSysSimpleColorModifier: three colours across the particle's life plus an
  // alpha fade in/out. When set, the sim tints/fades by this instead of the
  // fixed class colour. keys are life fractions (0 birth .. 1 death): fade-in
  // end, fade-out start, colour1 end, colour2 start, colour2 end, colour3 start.
  bool has_color_ramp = false;
  f32 ramp_color[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
  f32 ramp_key[6] = {0, 1, 0.33f, 0.33f, 0.66f, 0.66f};
};

struct Mesh {
  AssetId id;
  base::Vector<MeshLod> lods;
  f32 bounds_center[3] = {0, 0, 0};
  f32 bounds_radius = 0;
  // Kept out of acceleration structures: dense fill geometry like grass
  // would bloat the tlas and add ray noise without visual benefit.
  bool exclude_from_rt = false;
  // Distant terrain LOD proxy (.btr): sunk under the full-detail land where
  // cells are streamed in, so the coarse grid never bridges above it.
  bool terrain_lod = false;
  // Set when MeshLod::skinning is populated: the mesh skins on the GPU against
  // a skeleton matched by `skin.bones` names. Empty for static geometry.
  bool skinned = false;
  SkinBinding skin;
  // Particle emitters riding along with the mesh (NIF particle systems).
  base::Vector<ParticleEmitter> emitters;
};

}  // namespace rx::asset

#endif  // RX_ASSET_MESH_H_
