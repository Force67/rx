#ifndef RX_ASSET_SIMPLIFY_H_
#define RX_ASSET_SIMPLIFY_H_

// Quadric-error-metric mesh simplification (Garland-Heckbert) via half-edge
// collapses: a collapse moves vertex a onto vertex b, so no new positions are
// invented and locked vertices (cluster-group borders in the virtual-geometry
// DAG build) are preserved exactly - the property the crack-free LOD cut
// depends on.

#include <vector>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::asset {

// Simplifies `indices` (triangle list over `positions`) toward
// `target_index_count`. Vertices with locked[v] != 0 never move or disappear.
// Returns the simplified index list; *out_error receives the maximum collapse
// distance (world units), an upper-bound style error for LOD selection.
RX_ASSET_EXPORT std::vector<u32> SimplifyIndices(const rx::Vec3* positions, u32 vertex_count,
                                 const u32* indices, u32 index_count, u32 target_index_count,
                                 const u8* locked, f32* out_error);

}  // namespace rx::asset

#endif  // RX_ASSET_SIMPLIFY_H_
