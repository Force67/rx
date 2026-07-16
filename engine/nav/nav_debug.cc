#include "nav/nav_debug.h"

#include <cmath>

namespace rx::nav {
namespace {

constexpr f32 kLift = 0.06f;  // meters above the surface so lines never z-fight

// Area palette, 0xRRGGBBAA. Index 0 unused (holes draw nothing).
constexpr u32 kAreaColors[8] = {
    0x00000000,  // kAreaNone
    0x3fae4cb4,  // ground: green
    0xe08a2ab4,  // 2: rough / rock: orange
    0x2a72e0b4,  // 3: shallow water: blue
    0x1237a0b4,  // 4: deep water: navy
    0xb03ae0b4,  // 5: purple
    0xe0d02ab4,  // 6: yellow
    0xe02a5eb4,  // 7: red
};

u32 AreaColor(AreaId area) { return kAreaColors[area & 7]; }

}  // namespace

void AppendNavMeshLines(const NavMesh& mesh, const Vec3& center, f32 radius,
                        base::Vector<render::DebugLine>* out) {
  const f32 cs = mesh.config().cell_size;
  const i32 r = static_cast<i32>(std::ceil(radius / cs));
  const CellRef mid = mesh.CellAt(center);
  for (i32 dz = -r; dz <= r; ++dz) {
    for (i32 dx = -r; dx <= r; ++dx) {
      const CellRef cell{mid.x + dx, mid.z + dz};
      const AreaId area = mesh.Area(cell);
      if (area == kAreaNone) continue;
      const u32 color = AreaColor(area);
      const f32 x0 = static_cast<f32>(cell.x) * cs;
      const f32 z0 = static_cast<f32>(cell.z) * cs;
      const f32 y = mesh.CellCenter(cell).y + kLift;
      // Two edges per cell (west + south); neighbors complete the grid. The
      // outer rim misses two edges, invisible in practice and half the lines.
      out->push_back({{x0, y, z0}, {x0, y, z0 + cs}, color});
      out->push_back({{x0, y, z0}, {x0 + cs, y, z0}, color});
      // Cross-hatch cells whose area differs from plain ground so cost paint
      // reads at a glance even where colors blend.
      if (area != kAreaGround) {
        out->push_back({{x0, y, z0}, {x0 + cs, y, z0 + cs}, color});
      }
    }
  }
}

void AppendCorridorLines(const NavMesh& mesh, const Corridor& corridor,
                         base::Vector<render::DebugLine>* out) {
  if (corridor.cells.size() < 2) return;
  for (u32 i = 1; i < corridor.cells.size(); ++i) {
    Vec3 a = mesh.CellCenter(corridor.cells[i - 1]);
    Vec3 b = mesh.CellCenter(corridor.cells[i]);
    a.y += kLift * 2;
    b.y += kLift * 2;
    const bool ahead = i > corridor.progress;
    const u32 color = ahead ? 0xffffffff : 0x707070a0;
    out->push_back({a, b, color});
  }
}

void AppendAgentLines(const NavMesh& mesh, const Vec3& agent_pos, const NavAgent& agent,
                      base::Vector<render::DebugLine>* out) {
  if (agent.status != AgentStatus::kMoving) return;
  Vec3 from = agent_pos;
  Vec3 corner = agent.corner;
  from.y += kLift * 3;
  corner.y += kLift * 3;
  out->push_back({from, corner, 0x2ae0d0ff});  // cyan: live funnel corner
  Vec3 goal = agent.goal;
  f32 h;
  if (mesh.HeightAt(goal.x, goal.z, &h)) goal.y = h;
  Vec3 above = goal;
  above.y += 1.2f;
  out->push_back({goal, above, 0xe02a5eff});  // red post at the goal
}

}  // namespace rx::nav
