#include "nav/path.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rx::nav {
namespace {

u64 PackCell(CellRef c) {
  return (static_cast<u64>(static_cast<u32>(c.x)) << 32) | static_cast<u32>(c.z);
}

// Signed doubled area of (a, b, c) in the XZ plane, Recast convention:
// positive when c lies to the LEFT of a->b in a y-up right-handed world
// (facing +x, left is -z). The funnel and portal orientation share this.
f32 TriArea2(const Vec3& a, const Vec3& b, const Vec3& c) {
  return (c.x - a.x) * (b.z - a.z) - (b.x - a.x) * (c.z - a.z);
}

f32 PlanarDist(const Vec3& a, const Vec3& b) {
  const f32 dx = a.x - b.x;
  const f32 dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

f32 Octile(CellRef a, CellRef b, f32 cell_size) {
  const f32 dx = std::fabs(static_cast<f32>(a.x - b.x));
  const f32 dz = std::fabs(static_cast<f32>(a.z - b.z));
  const f32 lo = std::min(dx, dz);
  const f32 hi = std::max(dx, dz);
  return (hi + 0.41421356f * lo) * cell_size;
}

// Min-heap over scratch.nodes keyed by f = g + h.
void HeapPush(PathScratch& s, u32 node) {
  s.heap.push_back(node);
  u32 i = static_cast<u32>(s.heap.size()) - 1;
  while (i > 0) {
    const u32 parent = (i - 1) / 2;
    const PathScratch::Node& a = s.nodes[s.heap[i]];
    const PathScratch::Node& b = s.nodes[s.heap[parent]];
    if (a.g + a.h >= b.g + b.h) break;
    std::swap(s.heap[i], s.heap[parent]);
    i = parent;
  }
}

u32 HeapPop(PathScratch& s) {
  const u32 top = s.heap[0];
  s.heap[0] = s.heap.back();
  s.heap.pop_back();
  u32 i = 0;
  const u32 count = static_cast<u32>(s.heap.size());
  while (true) {
    const u32 l = i * 2 + 1;
    const u32 r = i * 2 + 2;
    u32 smallest = i;
    auto f = [&](u32 at) {
      const PathScratch::Node& n = s.nodes[s.heap[at]];
      return n.g + n.h;
    };
    if (l < count && f(l) < f(smallest)) smallest = l;
    if (r < count && f(r) < f(smallest)) smallest = r;
    if (smallest == i) break;
    std::swap(s.heap[i], s.heap[smallest]);
    i = smallest;
  }
  return top;
}

// Diagonal moves must not cut corners: both flanking cardinals need to be
// steppable too, or a running agent clips the blocked corner.
bool DiagonalAllowed(const NavMesh& mesh, CellRef from, CellRef to) {
  const CellRef a{to.x, from.z};
  const CellRef b{from.x, to.z};
  return mesh.Reachable(from, a) && mesh.Reachable(from, b);
}

void StampTiles(const NavMesh& mesh, Corridor* corridor) {
  corridor->tiles.clear();
  u64 last = ~0ull;
  for (const CellRef& cell : corridor->cells) {
    const u64 key = mesh.TileKeyOf(cell);
    if (key == last) continue;
    last = key;
    bool seen = false;
    for (const TileStamp& stamp : corridor->tiles) {
      if (stamp.key == key) {
        seen = true;
        break;
      }
    }
    if (!seen) corridor->tiles.push_back({key, mesh.TileVersionByKey(key)});
  }
}

f32 CorridorCost(const NavMesh& mesh, const base::Vector<CellRef>& cells) {
  f32 cost = 0;
  for (u32 i = 1; i < cells.size(); ++i) cost += mesh.StepCost(cells[i - 1], cells[i]);
  return cost;
}

// Re-walks the straight segment collecting the crossed cells; true only when
// every hop is steppable (the collecting twin of NavMesh::Raycast).
bool TraceCells(const NavMesh& mesh, const Vec3& from, const Vec3& to,
                base::Vector<CellRef>* out) {
  out->clear();
  const f32 cs = mesh.config().cell_size;
  CellRef cell = mesh.CellAt(from);
  const CellRef end = mesh.CellAt(to);
  if (!mesh.Walkable(cell)) return false;
  out->push_back(cell);
  const f32 dx = to.x - from.x;
  const f32 dz = to.z - from.z;
  const i32 step_x = dx > 0 ? 1 : -1;
  const i32 step_z = dz > 0 ? 1 : -1;
  const f32 inv_dx = dx != 0 ? 1.0f / dx : 0;
  const f32 inv_dz = dz != 0 ? 1.0f / dz : 0;
  f32 t_max_x = dx != 0 ? ((static_cast<f32>(cell.x + (step_x > 0)) * cs) - from.x) * inv_dx : 2.0f;
  f32 t_max_z = dz != 0 ? ((static_cast<f32>(cell.z + (step_z > 0)) * cs) - from.z) * inv_dz : 2.0f;
  const f32 t_delta_x = dx != 0 ? cs * static_cast<f32>(step_x) * inv_dx : 0;
  const f32 t_delta_z = dz != 0 ? cs * static_cast<f32>(step_z) * inv_dz : 0;
  const i32 max_steps = std::abs(end.x - cell.x) + std::abs(end.z - cell.z) + 2;
  for (i32 i = 0; i < max_steps && !(cell == end); ++i) {
    CellRef next = cell;
    if (t_max_x < t_max_z) {
      next.x += step_x;
      t_max_x += t_delta_x;
    } else {
      next.z += step_z;
      t_max_z += t_delta_z;
    }
    if (!mesh.Reachable(cell, next)) return false;
    out->push_back(next);
    cell = next;
  }
  return cell == end;
}

// One shortcutting sweep front-to-back. Returns the straightened cell list.
void ShortcutPass(const NavMesh& mesh, base::Vector<CellRef>* cells, u32 max_span,
                  base::Vector<CellRef>* scratch_line, base::Vector<CellRef>* scratch_out) {
  const u32 count = static_cast<u32>(cells->size());
  if (count < 3) return;
  base::Vector<CellRef>& out = *scratch_out;
  out.clear();
  out.push_back((*cells)[0]);
  base::Vector<f32> prefix;  // corridor cost cells[i] .. cells[j], reused
  u32 i = 0;
  while (i + 1 < count) {
    u32 chosen = i + 1;
    if (i + 2 < count) {
      const u32 far = std::min(i + max_span, count - 1);
      // Corridor cost of the stretch being challenged.
      f32 stretch_cost = 0;
      prefix.clear();
      prefix.push_back(0);
      for (u32 j = i + 1; j <= far; ++j) {
        stretch_cost += mesh.StepCost((*cells)[j - 1], (*cells)[j]);
        prefix.push_back(stretch_cost);
      }
      for (u32 j = far; j > i + 1; --j) {
        const Vec3 a = mesh.CellCenter((*cells)[i]);
        const Vec3 b = mesh.CellCenter((*cells)[j]);
        const NavRaycast ray = mesh.Raycast(a, b);
        if (!ray.clear) continue;
        if (ray.cost > prefix[j - i] + 1e-3f) continue;  // straight but pricier: keep corridor
        if (!TraceCells(mesh, a, b, scratch_line)) continue;
        for (u32 k = 1; k < scratch_line->size(); ++k) out.push_back((*scratch_line)[k]);
        chosen = j;
        break;
      }
    }
    if (chosen == i + 1 && out.back() != (*cells)[chosen]) out.push_back((*cells)[chosen]);
    i = chosen;
  }
  *cells = static_cast<base::Vector<CellRef>&&>(out);
}

// Portal between consecutive corridor cells, (left, right) w.r.t. travel.
struct Portal {
  Vec3 left;
  Vec3 right;
};

Portal MakePortal(const NavMesh& mesh, CellRef from, CellRef to, f32 radius) {
  const f32 cs = mesh.config().cell_size;
  const i32 dx = to.x - from.x;
  const i32 dz = to.z - from.z;
  const f32 y = mesh.CellCenter(to).y;
  if (dx != 0 && dz != 0) {
    // Diagonal: the shared corner, a degenerate portal.
    const f32 px = (static_cast<f32>(from.x) + (dx > 0 ? 1.0f : 0.0f)) * cs;
    const f32 pz = (static_cast<f32>(from.z) + (dz > 0 ? 1.0f : 0.0f)) * cs;
    return {{px, y, pz}, {px, y, pz}};
  }
  Vec3 a, b;
  if (dx != 0) {  // crossing a z-aligned edge
    const f32 px = (static_cast<f32>(from.x) + (dx > 0 ? 1.0f : 0.0f)) * cs;
    a = {px, y, static_cast<f32>(from.z) * cs};
    b = {px, y, (static_cast<f32>(from.z) + 1.0f) * cs};
  } else {  // crossing an x-aligned edge
    const f32 pz = (static_cast<f32>(from.z) + (dz > 0 ? 1.0f : 0.0f)) * cs;
    a = {static_cast<f32>(from.x) * cs, y, pz};
    b = {(static_cast<f32>(from.x) + 1.0f) * cs, y, pz};
  }
  // Orient the pair for the funnel's update conditions: its "left" is the
  // endpoint with NEGATIVE TriArea2 against the travel direction (the +z side
  // when walking +x).
  const Vec3 dir{static_cast<f32>(dx), 0, static_cast<f32>(dz)};
  const Vec3 mid = (a + b) * 0.5f;
  Portal portal;
  if (TriArea2({0, 0, 0}, dir, a - mid) < 0) {
    portal.left = a;
    portal.right = b;
  } else {
    portal.left = b;
    portal.right = a;
  }
  // Nudge the ends toward the middle so pulled corners keep some clearance,
  // but never enough to turn the portal into a forced waypoint: agent
  // clearance belongs in cell walkability (paint margins around obstacles),
  // not in portals a third the agent's width.
  const f32 half_len = cs * 0.5f;
  const f32 t = std::min(radius, half_len * 0.33f) / half_len;
  portal.left = Lerp(portal.left, mid, t);
  portal.right = Lerp(portal.right, mid, t);
  return portal;
}

// The live end point the funnel pulls toward: the exact goal while it sits in
// the corridor's final cell, else that cell's center.
Vec3 CorridorTarget(const NavMesh& mesh, const Corridor& corridor) {
  if (corridor.cells.empty()) return corridor.goal;
  const CellRef end = corridor.cells.back();
  if (corridor.status == PathStatus::kComplete && mesh.CellAt(corridor.goal) == end) {
    Vec3 goal = corridor.goal;
    f32 h;
    if (mesh.HeightAt(goal.x, goal.z, &h)) goal.y = h;
    return goal;
  }
  return mesh.CellCenter(end);
}

// Advances corridor->progress to the agent's cell. A cell of drift counts as
// on-corridor (the corridor is one cell wide; running agents cut corners).
bool AdvanceProgress(const NavMesh& mesh, Corridor* corridor, const Vec3& agent_pos) {
  const CellRef at = mesh.CellAt(agent_pos);
  const u32 count = static_cast<u32>(corridor->cells.size());
  const u32 window = 12;
  u32 best = UINT32_MAX;
  i32 best_d = INT32_MAX;
  const u32 lo = corridor->progress > 1 ? corridor->progress - 1 : 0;
  for (u32 k = lo; k < count && k <= corridor->progress + window; ++k) {
    const CellRef c = corridor->cells[k];
    const i32 d = std::max(std::abs(c.x - at.x), std::abs(c.z - at.z));
    // Prefer the farthest matching cell so progress is monotonic even when
    // the agent straddles several corridor cells.
    if (d <= best_d && d <= 1) {
      best_d = d;
      best = k;
    }
  }
  if (best == UINT32_MAX) return false;
  corridor->progress = std::max(corridor->progress, best);
  return true;
}

}  // namespace

PathStatus FindPath(const NavMesh& mesh, const PathRequest& request, PathScratch& scratch,
                    Corridor* out) {
  out->cells.clear();
  out->tiles.clear();
  out->goal = request.goal;
  out->goal_cell = mesh.CellAt(request.goal);
  out->clamped_goal = {};
  out->status = PathStatus::kFailed;
  out->cost = 0;
  out->progress = 0;

  const CellRef start = mesh.ClampToWalkable(request.start, request.clamp_radius);
  if (!start.valid()) return PathStatus::kFailed;
  CellRef target = mesh.Walkable(out->goal_cell)
                       ? out->goal_cell
                       : mesh.ClampToWalkable(request.goal, request.clamp_radius);
  out->clamped_goal = target;
  // With no reachable target cell at all (goal far off the built bubble),
  // search greedily toward the raw goal cell and take the partial result.
  const CellRef heuristic_target = target.valid() ? target : out->goal_cell;

  const f32 cs = mesh.config().cell_size;
  scratch.nodes.clear();
  scratch.heap.clear();
  scratch.visited.clear();

  scratch.nodes.push_back({start, 0, Octile(start, heuristic_target, cs), -1});
  scratch.visited[PackCell(start)] = 0;
  HeapPush(scratch, 0);

  i32 end_node = -1;
  u32 best_partial = 0;  // node nearest the goal by heuristic, g breaks ties
  u32 iterations = 0;
  while (!scratch.heap.empty() && iterations < request.max_iterations) {
    ++iterations;
    const u32 current = HeapPop(scratch);
    const PathScratch::Node node = scratch.nodes[current];  // copy: nodes grows below
    if (target.valid() && node.cell == target) {
      end_node = static_cast<i32>(current);
      break;
    }
    {
      const PathScratch::Node& best = scratch.nodes[best_partial];
      if (node.h < best.h || (node.h == best.h && node.g < best.g)) best_partial = current;
    }
    for (i32 dz = -1; dz <= 1; ++dz) {
      for (i32 dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dz == 0) continue;
        const CellRef next{node.cell.x + dx, node.cell.z + dz};
        if (!mesh.Reachable(node.cell, next)) continue;
        if (dx != 0 && dz != 0 && !DiagonalAllowed(mesh, node.cell, next)) continue;
        const f32 g = node.g + mesh.StepCost(node.cell, next);
        const u64 key = PackCell(next);
        u32* seen = scratch.visited.find(key);
        if (seen) {
          PathScratch::Node& other = scratch.nodes[*seen];
          if (g >= other.g) continue;
          other.g = g;
          other.parent = static_cast<i32>(current);
          HeapPush(scratch, *seen);  // decrease-key via re-push; stale dups skip
        } else {
          const u32 index = static_cast<u32>(scratch.nodes.size());
          scratch.nodes.push_back(
              {next, g, Octile(next, heuristic_target, cs), static_cast<i32>(current)});
          scratch.visited[key] = index;
          HeapPush(scratch, index);
        }
      }
    }
  }

  const bool complete = end_node >= 0;
  const u32 last = complete ? static_cast<u32>(end_node) : best_partial;
  for (i32 walk = static_cast<i32>(last); walk >= 0; walk = scratch.nodes[walk].parent) {
    out->cells.push_back(scratch.nodes[walk].cell);
  }
  std::reverse(out->cells.begin(), out->cells.end());
  out->cost = scratch.nodes[last].g;
  out->status = complete ? PathStatus::kComplete : PathStatus::kPartial;
  // A partial "path" that never left the start cell helps nobody: fail so the
  // caller waits for tiles instead of celebrating zero progress.
  if (!complete && out->cells.size() <= 1 && !(start == heuristic_target)) {
    out->status = PathStatus::kFailed;
  }
  if (request.shortcut && out->status != PathStatus::kFailed) {
    ShortcutCorridor(mesh, out);
  } else if (out->status != PathStatus::kFailed) {
    StampTiles(mesh, out);
  }
  return out->status;
}

void ShortcutCorridor(const NavMesh& mesh, Corridor* corridor, u32 max_span) {
  if (corridor->cells.size() >= 3) {
    base::Vector<CellRef> line, scratch;
    ShortcutPass(mesh, &corridor->cells, max_span, &line, &scratch);
    // Second sweep from the far end: reverse, straighten, restore. Entry
    // costs are direction-sensitive, so the reversed comparison is
    // approximate; the epsilon in ShortcutPass keeps it conservative.
    std::reverse(corridor->cells.begin(), corridor->cells.end());
    ShortcutPass(mesh, &corridor->cells, max_span, &line, &scratch);
    std::reverse(corridor->cells.begin(), corridor->cells.end());
    corridor->cost = CorridorCost(mesh, corridor->cells);
  }
  StampTiles(mesh, corridor);
}

void FunnelCorners(const NavMesh& mesh, const Corridor& corridor, const Vec3& from,
                   u32 start_index, f32 radius, u32 max_corners,
                   base::Vector<Vec3>* out_corners) {
  out_corners->clear();
  const u32 count = static_cast<u32>(corridor.cells.size());
  const Vec3 target = CorridorTarget(mesh, corridor);
  if (start_index + 1 >= count) {
    out_corners->push_back(target);
    return;
  }

  // Portals for every remaining transition, then the goal as a degenerate one.
  base::Vector<Portal> portals;
  portals.reserve(count - start_index);
  for (u32 i = start_index; i + 1 < count; ++i) {
    portals.push_back(MakePortal(mesh, corridor.cells[i], corridor.cells[i + 1], radius));
  }
  portals.push_back({target, target});

  // Simple stupid funnel.
  Vec3 apex = from;
  Vec3 left = portals[0].left;
  Vec3 right = portals[0].right;
  u32 left_index = 0, right_index = 0;
  for (u32 i = 1; i < portals.size() && out_corners->size() < max_corners; ++i) {
    const Vec3& new_left = portals[i].left;
    const Vec3& new_right = portals[i].right;
    // Tighten the right side.
    if (TriArea2(apex, right, new_right) <= 0) {
      if (PlanarDist(apex, right) < 1e-4f || TriArea2(apex, left, new_right) > 0) {
        right = new_right;
        right_index = i;
      } else {
        out_corners->push_back(left);  // left corner turns the funnel
        apex = left;
        i = left_index;
        left = apex;
        right = apex;
        left_index = right_index = i;
        continue;
      }
    }
    // Tighten the left side.
    if (TriArea2(apex, left, new_left) >= 0) {
      if (PlanarDist(apex, left) < 1e-4f || TriArea2(apex, right, new_left) < 0) {
        left = new_left;
        left_index = i;
      } else {
        out_corners->push_back(right);
        apex = right;
        i = right_index;
        left = apex;
        right = apex;
        left_index = right_index = i;
        continue;
      }
    }
  }
  // The goal is always the final corner (unless the corner budget is spent);
  // skip the push when the funnel already emitted it.
  if (out_corners->size() < max_corners &&
      (out_corners->empty() || PlanarDist(out_corners->back(), target) > 1e-4f)) {
    out_corners->push_back(target);
  }
}

bool NextCorner(const NavMesh& mesh, Corridor* corridor, const Vec3& agent_pos, f32 radius,
                Vec3* out_corner) {
  if (!corridor->valid()) return false;
  if (!AdvanceProgress(mesh, corridor, agent_pos)) return false;
  base::Vector<Vec3> corners;
  FunnelCorners(mesh, *corridor, agent_pos, corridor->progress, radius, 8, &corners);
  for (const Vec3& corner : corners) {
    if (PlanarDist(corner, agent_pos) > 1e-3f) {
      *out_corner = corner;
      return true;
    }
  }
  *out_corner = CorridorTarget(mesh, *corridor);
  return true;
}

RepathReason ValidateCorridor(const NavMesh& mesh, Corridor* corridor, const Vec3& agent_pos,
                              const Vec3& goal) {
  if (!corridor->valid()) return RepathReason::kNoCorridor;

  // 1. The world changed under the corridor (tile rebuilt, painted, dropped).
  for (const TileStamp& stamp : corridor->tiles) {
    if (mesh.TileVersionByKey(stamp.key) != stamp.version) {
      return RepathReason::kTileInvalidated;
    }
  }

  // 2. The agent is no longer inside the corridor.
  if (!AdvanceProgress(mesh, corridor, agent_pos)) return RepathReason::kLeftCorridor;

  // 3/4. The destination moved: outside its planned cell when on-mesh, or its
  // nearest-walkable clamp changed when off-mesh. Same-cell drift only
  // retargets the funnel endpoint; no search runs.
  const CellRef goal_cell = mesh.CellAt(goal);
  if (!(goal_cell == corridor->goal_cell)) {
    if (mesh.Walkable(goal_cell)) return RepathReason::kGoalCellChanged;
    const CellRef clamp = mesh.ClampToWalkable(goal, 4.0f);
    if (!(clamp == corridor->clamped_goal)) return RepathReason::kClampedGoalChanged;
    corridor->goal_cell = goal_cell;  // moved off-mesh, same clamp: keep going
  }
  corridor->goal = goal;

  // 5. Reached the end of an incomplete path: plan the next stretch.
  if (corridor->status == PathStatus::kPartial &&
      corridor->progress + 1 >= corridor->cells.size()) {
    return RepathReason::kEndOfPartialPath;
  }
  return RepathReason::kNone;
}

}  // namespace rx::nav
