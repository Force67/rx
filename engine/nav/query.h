#ifndef RX_NAV_QUERY_H_
#define RX_NAV_QUERY_H_

// Terrain-aware position selection. A combat position that is geometrically
// close can still be a terrible choice when a river runs between: the
// spatial query floods weighted path costs outward (Dijkstra) and scores each
// candidate by DELTA COST -- weighted cost minus raw path length, i.e. how
// much of the route runs through undesirable terrain, independent of how far
// it is. Deltas are normalized to a shared virtual length: every route is
// padded out to virtual_length meters as if the agent then stood on the
// destination terrain, so the score compares equal time windows. Without
// that, an agent already standing in a river would rank "stand still" (delta
// 0) above every move, since any path it can take touches water.

#include <span>

#include <base/containers/vector.h>

#include "core/export.h"
#include "nav/navmesh.h"
#include "nav/path.h"

namespace rx::nav {

struct PositionQueryParams {
  Vec3 origin;                // flood source: the agent's position
  f32 max_cost = 80.0f;       // weighted-meter budget; flood stops beyond it
  u32 max_iterations = 4096;  // Dijkstra pop cap
  f32 virtual_length = 20.0f; // shared normalization length for delta costs
  f32 clamp_radius = 2.0f;    // candidates snap to walkable within this
};

struct PositionCandidate {
  Vec3 position;         // in: the spot being scored (out: clamped to mesh)
  bool reachable = false;
  f32 raw_length = 0;    // unweighted meters along the flooded route
  f32 weighted_cost = 0; // multiplier + entry-cost meters along it
  f32 delta_cost = 0;    // weighted - raw: meters "wasted" in bad terrain
  f32 score = 0;         // normalized delta; lower is better
};

// Floods from params.origin, then fills every candidate in place. Returns the
// index of the best reachable candidate (lowest score, raw_length breaks
// ties), or SIZE_MAX when none are reachable. The flood is one search shared
// by all candidates -- adding candidates is nearly free.
RX_NAV_EXPORT size_t EvaluatePositions(const NavMesh& mesh, const PositionQueryParams& params,
                                       std::span<PositionCandidate> candidates);

}  // namespace rx::nav

#endif  // RX_NAV_QUERY_H_
