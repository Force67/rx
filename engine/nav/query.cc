#include "nav/query.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rx::nav {
namespace {

u64 PackCell(CellRef c) {
  return (static_cast<u64>(static_cast<u32>(c.x)) << 32) | static_cast<u32>(c.z);
}

struct FloodNode {
  CellRef cell;
  f32 weighted = 0;  // Dijkstra key
  f32 raw = 0;       // unweighted meters along the same route
};

struct FloodHeap {
  base::Vector<FloodNode> items;

  void Push(const FloodNode& n) {
    items.push_back(n);
    u32 i = static_cast<u32>(items.size()) - 1;
    while (i > 0) {
      const u32 p = (i - 1) / 2;
      if (items[i].weighted >= items[p].weighted) break;
      std::swap(items[i], items[p]);
      i = p;
    }
  }

  FloodNode Pop() {
    FloodNode top = items[0];
    items[0] = items.back();
    items.pop_back();
    u32 i = 0;
    const u32 count = static_cast<u32>(items.size());
    while (true) {
      const u32 l = i * 2 + 1, r = i * 2 + 2;
      u32 m = i;
      if (l < count && items[l].weighted < items[m].weighted) m = l;
      if (r < count && items[r].weighted < items[m].weighted) m = r;
      if (m == i) break;
      std::swap(items[i], items[m]);
      i = m;
    }
    return top;
  }
};

bool DiagonalAllowed(const NavMesh& mesh, CellRef from, CellRef to) {
  return mesh.Reachable(from, {to.x, from.z}) && mesh.Reachable(from, {from.x, to.z});
}

}  // namespace

size_t EvaluatePositions(const NavMesh& mesh, const PositionQueryParams& params,
                         std::span<PositionCandidate> candidates) {
  const CellRef source = mesh.ClampToWalkable(params.origin, params.clamp_radius * 2.0f);
  for (PositionCandidate& c : candidates) c.reachable = false;
  if (!source.valid() || candidates.empty()) return SIZE_MAX;

  // One flood serves every candidate: settle weighted (and raw) cost per cell
  // until the budget or the pop cap runs out.
  struct Settled {
    f32 weighted;
    f32 raw;
    bool done;
  };
  base::UnorderedMap<u64, Settled> best;
  FloodHeap heap;
  heap.Push({source, 0, 0});
  best[PackCell(source)] = {0, 0, false};

  const f32 cs = mesh.config().cell_size;
  u32 pops = 0;
  while (!heap.items.empty() && pops < params.max_iterations) {
    ++pops;
    const FloodNode node = heap.Pop();
    Settled* settled = best.find(PackCell(node.cell));
    if (settled && settled->done) continue;  // stale duplicate
    if (settled) settled->done = true;
    if (node.weighted > params.max_cost) break;  // heap is min-ordered: all done
    for (i32 dz = -1; dz <= 1; ++dz) {
      for (i32 dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dz == 0) continue;
        const CellRef next{node.cell.x + dx, node.cell.z + dz};
        if (!mesh.Reachable(node.cell, next)) continue;
        if (dx != 0 && dz != 0 && !DiagonalAllowed(mesh, node.cell, next)) continue;
        const f32 weighted = node.weighted + mesh.StepCost(node.cell, next);
        if (weighted > params.max_cost) continue;
        const f32 raw = node.raw + ((dx != 0 && dz != 0) ? cs * 1.41421356f : cs);
        const u64 key = PackCell(next);
        Settled* seen = best.find(key);
        if (seen && seen->weighted <= weighted) continue;
        best[key] = {weighted, raw, false};
        heap.Push({next, weighted, raw});
      }
    }
  }

  // Score the candidates off the settled field.
  size_t winner = SIZE_MAX;
  for (size_t i = 0; i < candidates.size(); ++i) {
    PositionCandidate& c = candidates[i];
    const CellRef cell = mesh.ClampToWalkable(c.position, params.clamp_radius);
    if (!cell.valid()) continue;
    const Settled* settled = best.find(PackCell(cell));
    if (!settled) continue;
    c.position = mesh.CellCenter(cell);
    c.reachable = true;
    c.raw_length = settled->raw;
    c.weighted_cost = settled->weighted;
    c.delta_cost = settled->weighted - settled->raw;
    // Normalize to the shared virtual length: pad every route out to
    // virtual_length meters as if the agent kept standing on the destination
    // terrain. The score then reads "weighted meters wasted in bad terrain
    // over the same time window", so an agent already in the river is pushed
    // toward the bank (staying put pads at water cost) instead of freezing
    // because every move would add delta.
    const f32 pad = std::max(params.virtual_length - settled->raw, 0.0f);
    c.score = c.delta_cost + pad * (mesh.TraverseMultiplier(mesh.Area(cell)) - 1.0f);
    if (winner == SIZE_MAX || c.score < candidates[winner].score ||
        (c.score == candidates[winner].score && c.raw_length < candidates[winner].raw_length)) {
      winner = i;
    }
  }
  return winner;
}

}  // namespace rx::nav
