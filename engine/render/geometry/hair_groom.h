#ifndef RX_RENDER_HAIR_GROOM_H_
#define RX_RENDER_HAIR_GROOM_H_

#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "asset/texture.h"
#include "core/math.h"

namespace rx::render {

// Guide-strand grooming from an authored hair mesh. Hair card meshes are
// (shell) sheets whose UVs run root->tip along one axis; we resample each card
// into strands that trace it from the scalp edge to the tip, so the groom keeps
// the original silhouette instead of dissolving into a fuzzball.

static constexpr u32 kGroomPointsPerStrand = 16;

struct GroomParams {
  u32 guide_count = 1600;         // simulated guide strands
  u32 children_per_guide = 6;     // rendered clump children per guide (M > N)
  f32 clump_radius = 0.006f;      // child clump radius at the root (engine units)
  f32 strand_width = 0.0016f;     // ribbon half-width at the root (engine units)
  Vec3 tint{1, 1, 1};             // multiplies the sampled diffuse colour
  const asset::Texture* diffuse = nullptr;  // hair diffuse for per-strand colour
  u32 seed = 1;
  // Scale that converts the source mesh's authored units into engine metres
  // (the groom is built in engine space, Y-up). Default 1 assumes the mesh is
  // already in engine metres; a caller whose content is authored in game units
  // passes that content's unit->metre scale.
  f32 units_to_meters = 1.0f;
  // Scales the per-strand flyaway + curl amplitude. 1 = the demo's loose,
  // wind-blown look; lower values keep strands tight to the card silhouette for
  // groomed styles (facegen heads read as spikes at full frizz).
  f32 frizz = 1.0f;
  // When true (the default) the groom is recentred so the scalp sits at the local
  // origin, ready for a translation. Bone-attached hair keeps its authored
  // (engine-scaled, head-local) coordinates instead, so the caller can drop it on
  // the head bone with the head part's own transform.
  bool recenter = true;
};

// Per-style simulation feel, carried by the groom data and handed to the
// physics strand sim (physics::PhysicsWorld::StrandGroomDesc mirrors these
// fields; compliance is inverse stiffness, 0 = rigid).
struct GroomSimParams {
  f32 stretch_compliance = 0;
  f32 bend_compliance = 0.02f;
  f32 bind_compliance = 1e-5f;
  f32 damping = 0.15f;
  f32 gravity_factor = 1.0f;
  f32 node_mass = 0.02f;
  f32 node_radius = 0.0015f;
  f32 max_stretch = 1.03f;
  u32 iterations = 5;
};

// A CPU-built groom in a groom-local frame: engine units, Y-up, recentred so the
// scalp sits at the local origin. A root transform drops it onto a head bone;
// `collision_*` is that head sphere in the same local frame. Hairstyles are
// data: `pins` fixes extra mid-strand nodes in the groom frame (a ponytail
// tie, bun pinning), `binds` weaves nodes of different strands together
// (braids, dreadlocks), `sim` sets the per-style feel.
struct GroomData {
  base::Vector<f32> points;  // guide_count * kGroomPointsPerStrand * 3 (xyz)
  base::Vector<f32> roots;   // guide_count * 3 (local root pos, for sim reset)
  base::Vector<f32> colors;  // guide_count * 3 (linear rgb)
  base::Vector<u32> pins;    // (strand, point) pairs pinned to the groom frame
  base::Vector<u32> binds;   // (strand_a, point_a, strand_b, point_b) ties
  GroomSimParams sim;
  u32 guide_count = 0;
  Vec3 collision_center{0, 0, 0};
  f32 collision_radius = 0;
  f32 mean_length = 0;  // average strand length, for diagnostics
  Vec3 authored_scalp{0, 0, 0};  // scalp mean before recentring (local engine units)
};

// Traces guide strands along the hair cards. Returns false when the mesh has no
// usable geometry.
bool BuildHairGroom(const asset::Mesh& hair_mesh, const GroomParams& params, GroomData* out);

// Procedural hairstyle grooms exercising the full style data model: loose
// long hair (roots only), a three-bundle braid woven with cross-strand binds,
// and a ponytail whose strands are pinned mid-strand at a gather point. Used
// by the strands demo and the strand-sim regression test.
enum class TestGroomStyle { kLoose, kBraid, kPonytail };
bool BuildTestGroom(TestGroomStyle style, u32 guide_count, u32 seed, GroomData* out);

}  // namespace rx::render

#endif  // RX_RENDER_HAIR_GROOM_H_
