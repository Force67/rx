#ifndef RX_NAV_AGENT_H_
#define RX_NAV_AGENT_H_

// ECS navigation agents. NavAgent is the game-facing component: set `goal`,
// read `velocity` / `status`. NavCorridor is the paired working state (the
// live corridor + funnel scratch); UpdateAgents adds it on demand.
//
// UpdateAgents is the per-tick system. Register it from the app:
//
//   scheduler.AddSystem(ecs::Stage::kSim, "nav_agents",
//       [&mesh](ecs::World& world, f32 dt) { nav::UpdateAgents(world, mesh, {}, dt); });
//
// Every frame each agent validates its corridor (event-based repathing: no
// timers), replans at most `max_repaths` corridors per tick across the whole
// world (first come, first served in iteration order; agents that miss the
// budget keep steering along their stale corridor and usually catch up next
// tick, since handled events do not fire again -- incremental progress beats
// stalling),
// re-runs the funnel from its live position and writes the desired planar
// velocity. With `move` set it also integrates the Transform and snaps it to
// the surface, which is all a demo or a simple game needs; games with their
// own movers read `velocity` and integrate themselves.

#include "core/export.h"
#include "ecs/world.h"
#include "nav/path.h"

namespace rx::nav {

enum class AgentStatus : u8 {
  kIdle = 0,     // no goal, or goal reached
  kMoving,
  kWaiting,      // no path found this tick (off-mesh, unreachable, no budget)
};

struct NavAgent {
  Vec3 goal{};
  f32 speed = 3.0f;          // meters/second along the corridor
  f32 radius = 0.3f;         // shrinks funnel portals; keeps corners off walls
  f32 arrive_radius = 0.4f;  // planar distance that counts as "there"
  bool active = false;       // set with goal; cleared on arrival
  // outputs, written by UpdateAgents each tick
  Vec3 velocity{};           // desired planar velocity (y stays 0; height comes
                             // from the surface snap when `move` is set)
  Vec3 corner{};             // the corner currently steered at
  AgentStatus status = AgentStatus::kIdle;
  RepathReason last_repath = RepathReason::kNone;  // why the last replan ran
  u32 repath_count = 0;
};

// Working state, one per agent; UpdateAgents attaches it lazily. A component
// so it lives and dies with the entity.
struct NavCorridor {
  Corridor corridor;
  PathStatus last_plan = PathStatus::kFailed;
};

struct AgentUpdateConfig {
  u32 max_repaths = 4;       // corridor replans per tick, across all agents
  u32 astar_iterations = 500;
  f32 clamp_radius = 4.0f;
  bool move = true;          // integrate Transform + snap to surface height
};

// Plans, validates and steers every NavAgent in the world. Uses one shared
// PathScratch internally; call from one thread (the sim stage).
RX_NAV_EXPORT void UpdateAgents(ecs::World& world, NavMesh& mesh, const AgentUpdateConfig& config,
                                f32 dt);

}  // namespace rx::nav

#endif  // RX_NAV_AGENT_H_
