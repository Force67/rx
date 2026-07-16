#ifndef RX_NAV_PATH_H_
#define RX_NAV_PATH_H_

// Pathfinding over the tiled navmesh, and the corridor machinery that keeps
// an agent honest about the path it is on:
//
//  - FindPath: capped A* (default 500 pops, the Death Stranding budget) with
//    per-area traversal multipliers and one-time entry costs. When the cap or
//    a navmesh hole stops the search short, the best partial path toward the
//    goal is returned instead of nothing, so agents make incremental progress
//    through a world whose tiles may not exist yet.
//  - ShortcutCorridor: navmesh raycasts from both ends replace edge-midpoint
//    detours with straighter, cheaper stretches.
//  - FunnelCorners / NextCorner: the funnel (string pulling) re-run from the
//    agent's CURRENT position every frame. A running agent that drifts inside
//    the corridor keeps getting a sensible next corner instead of steering at
//    a stale waypoint, missing the turn and repathing forever.
//  - ValidateCorridor: event-based repathing. Nobody repaths on a timer; a
//    corridor is replaced only when one of the RepathReason events fires.

#include <base/containers/vector.h>

#include "core/export.h"
#include "nav/navmesh.h"

namespace rx::nav {

enum class PathStatus : u8 {
  kFailed = 0,   // start unclampable or search found nothing at all
  kComplete,     // corridor ends in the (clamped) goal cell
  kPartial,      // budget or holes stopped short; corridor ends nearest-known
};

// Why a corridor must be rebuilt. Ordered roughly by urgency; kNone means the
// corridor is still valid this frame.
enum class RepathReason : u8 {
  kNone = 0,
  kNoCorridor,          // never had a path for the current goal
  kTileInvalidated,     // a tile under the corridor was rebuilt, painted away
  kLeftCorridor,        // the agent drifted off the corridor cells
  kGoalCellChanged,     // on-mesh destination moved outside the final cell
  kClampedGoalChanged,  // off-mesh destination's nearest-walkable cell moved
  kEndOfPartialPath,    // reached the end of an incomplete path; extend it
};

struct PathRequest {
  Vec3 start;
  Vec3 goal;
  u32 max_iterations = 500;  // A* pops before settling for a partial path
  f32 clamp_radius = 4.0f;   // how far off-mesh endpoints may be pulled in
  bool shortcut = true;      // run ShortcutCorridor on the result
};

// A tile the corridor crosses, stamped with the version seen at path time.
struct TileStamp {
  u64 key = 0;
  u32 version = 0;
};

// The path corridor: the cell sequence A* settled on plus everything needed
// to validate it event-style each frame without touching the search again.
struct Corridor {
  base::Vector<CellRef> cells;      // start cell .. end cell
  base::Vector<TileStamp> tiles;    // unique tiles crossed, stamped
  Vec3 goal{};                      // requested world goal at path time
  CellRef goal_cell;                // CellAt(goal) at path time
  CellRef clamped_goal;             // walkable cell the goal resolved to
  f32 clamp_radius = 4.0f;          // request's clamp radius, for validation
  PathStatus status = PathStatus::kFailed;
  f32 cost = 0;                     // weighted cost of the corridor
  u32 progress = 0;                 // corridor index the agent last occupied
  // Whether the agent has been on the corridor at least once. Until then an
  // off-corridor position is an approach (the clamped start can be up to
  // clamp_radius away), not a departure -- no kLeftCorridor, and NextCorner
  // steers at the corridor start instead of failing.
  bool entered = false;

  bool valid() const { return status != PathStatus::kFailed && !cells.empty(); }
};

// Reusable A* scratch. Hold one per pathfinding thread and hand it to every
// FindPath call: the open heap, node pool and visited map keep their capacity
// across searches instead of reallocating.
struct PathScratch {
  struct Node {
    CellRef cell;
    f32 g = 0;       // weighted cost from start
    f32 h = 0;       // octile heuristic to goal
    i32 parent = -1;
    bool closed = false;  // popped once; later heap duplicates are skipped
  };
  base::Vector<Node> nodes;
  base::Vector<u32> heap;                  // node indices, min-ordered by g+h
  base::UnorderedMap<u64, u32> visited;    // packed cell -> node index
};

// Plans start->goal. Returns the corridor by value semantics into *out (its
// vectors are reused). PathStatus doubles as the return value.
RX_NAV_EXPORT PathStatus FindPath(const NavMesh& mesh, const PathRequest& request,
                                  PathScratch& scratch, Corridor* out);

// Raycast shortcutting from both ends: whenever a straight run across the
// grid is passable and no more expensive than the corridor stretch it spans,
// the stretch is replaced by the straight cells. max_span bounds the lookahead
// per splice so the pass stays linear.
RX_NAV_EXPORT void ShortcutCorridor(const NavMesh& mesh, Corridor* corridor, u32 max_span = 24);

// String-pulls the corridor from `from` (the agent's live position, projected
// onto corridor index start_index) to its end. Portal edges shrink by radius.
// Appends at most max_corners world-space corners; the goal is always last.
RX_NAV_EXPORT void FunnelCorners(const NavMesh& mesh, const Corridor& corridor, const Vec3& from,
                                 u32 start_index, f32 radius, u32 max_corners,
                                 base::Vector<Vec3>* out_corners);

// The per-frame steering query: advances corridor->progress to the agent's
// position, re-runs the funnel from there and returns the next corner (the
// goal itself when arrived). While the agent has never been on the corridor
// (fresh plan from a clamped start), returns the corridor start so off-mesh
// agents walk back onto it. Returns false only when the agent WAS on the
// corridor and drifted off (caller should repath).
RX_NAV_EXPORT bool NextCorner(const NavMesh& mesh, Corridor* corridor, const Vec3& agent_pos,
                              f32 radius, Vec3* out_corner);

// Event-based path validation, run every frame instead of repathing on a
// timer. Also advances corridor->progress. `goal` is the CURRENT destination
// (it may have moved since the corridor was planned).
RX_NAV_EXPORT RepathReason ValidateCorridor(const NavMesh& mesh, Corridor* corridor,
                                            const Vec3& agent_pos, const Vec3& goal);

}  // namespace rx::nav

#endif  // RX_NAV_PATH_H_
