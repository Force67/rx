#ifndef RX_NAV_NAV_DEBUG_H_
#define RX_NAV_NAV_DEBUG_H_

// Debug-line builders for the navmesh, corridors and funnel corners. They
// only append render::DebugLine records; the app owns the vector and hands it
// to FrameView::debug_lines in OnBuildView. Separate target (rx::nav_viz) so
// headless servers link rx::nav without the renderer.

#include <base/containers/vector.h>

#include "core/export.h"
#include "nav/agent.h"
#include "nav/path.h"
#include "render/core/renderer.h"

namespace rx::nav {

// Cell outlines colored by area (kAreaGround green, water blue, rough orange,
// custom areas from a fixed palette), lifted slightly off the surface. Only
// cells within `radius` of `center` are drawn; a full bubble is tens of
// thousands of lines, keep the radius tight.
RX_NAV_VIZ_EXPORT void AppendNavMeshLines(const NavMesh& mesh, const Vec3& center, f32 radius,
                                          base::Vector<render::DebugLine>* out);

// The corridor as a polyline through its cell centers (dim past `progress`,
// bright ahead of it).
RX_NAV_VIZ_EXPORT void AppendCorridorLines(const NavMesh& mesh, const Corridor& corridor,
                                           base::Vector<render::DebugLine>* out);

// The live funnel result: agent position to first corner to goal.
RX_NAV_VIZ_EXPORT void AppendAgentLines(const NavMesh& mesh, const Vec3& agent_pos,
                                        const NavAgent& agent,
                                        base::Vector<render::DebugLine>* out);

}  // namespace rx::nav

#endif  // RX_NAV_NAV_DEBUG_H_
