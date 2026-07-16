#include "nav/agent.h"

#include <cmath>

#include "scene/components.h"

namespace rx::nav {
namespace {

Vec3 ReadPosition(const scene::Transform& t) {
  return {t.position[0], t.position[1], t.position[2]};
}

f32 PlanarDist(const Vec3& a, const Vec3& b) {
  const f32 dx = a.x - b.x;
  const f32 dz = a.z - b.z;
  return std::sqrt(dx * dx + dz * dz);
}

// Face the move direction: yaw-only quaternion around +Y. The engine is
// right-handed, +Z forward.
void FaceVelocity(scene::Transform& t, const Vec3& velocity) {
  const f32 planar = velocity.x * velocity.x + velocity.z * velocity.z;
  if (planar < 1e-6f) return;
  const f32 yaw = std::atan2(velocity.x, velocity.z);
  t.rotation[0] = 0;
  t.rotation[1] = std::sin(yaw * 0.5f);
  t.rotation[2] = 0;
  t.rotation[3] = std::cos(yaw * 0.5f);
}

}  // namespace

void UpdateAgents(ecs::World& world, NavMesh& mesh, const AgentUpdateConfig& config, f32 dt) {
  // One scratch shared by every replan this tick; capacity persists across
  // frames so steady-state planning does not allocate.
  static thread_local PathScratch scratch;

  // Pass 1: agents that need working state (structural change is deferred out
  // of the Each: adding components mid-iteration can relocate archetypes).
  base::Vector<ecs::Entity> missing;
  world.Each<NavAgent, scene::Transform>(
      [&](ecs::Entity entity, NavAgent& agent, scene::Transform&) {
        if (agent.active && !world.Has<NavCorridor>(entity)) missing.push_back(entity);
      });
  for (ecs::Entity entity : missing) world.Add(entity, NavCorridor{});

  u32 repath_budget = config.max_repaths;
  world.Each<NavAgent, NavCorridor, scene::Transform>([&](ecs::Entity, NavAgent& agent,
                                                          NavCorridor& state,
                                                          scene::Transform& transform) {
    agent.velocity = {};
    if (!agent.active) {
      agent.status = AgentStatus::kIdle;
      return;
    }
    const Vec3 pos = ReadPosition(transform);

    // Arrival check against the live goal, not the corridor: a moving goal
    // that walks into the agent still counts.
    if (PlanarDist(pos, agent.goal) <= agent.arrive_radius) {
      agent.active = false;
      agent.status = AgentStatus::kIdle;
      state.corridor.cells.clear();
      state.corridor.status = PathStatus::kFailed;
      return;
    }

    // Event-based repathing: replan only when validation names a reason, and
    // only within this tick's shared budget.
    const RepathReason reason = ValidateCorridor(mesh, &state.corridor, pos, agent.goal);
    if (reason != RepathReason::kNone && repath_budget > 0) {
      --repath_budget;
      PathRequest request;
      request.start = pos;
      request.goal = agent.goal;
      request.max_iterations = config.astar_iterations;
      request.clamp_radius = config.clamp_radius;
      state.last_plan = FindPath(mesh, request, scratch, &state.corridor);
      agent.last_repath = reason;
      ++agent.repath_count;
    }
    if (!state.corridor.valid()) {
      agent.status = AgentStatus::kWaiting;
      return;
    }

    // Per-frame funnel from the live position; the corner is regenerated, not
    // chased from a stale list.
    Vec3 corner;
    if (!NextCorner(mesh, &state.corridor, pos, agent.radius, &corner)) {
      agent.status = AgentStatus::kWaiting;  // off corridor; next tick repaths
      return;
    }
    agent.corner = corner;
    const Vec3 to_corner{corner.x - pos.x, 0, corner.z - pos.z};
    const f32 dist = std::sqrt(Dot(to_corner, to_corner));
    if (dist < 1e-4f) {
      agent.status = AgentStatus::kMoving;
      return;
    }
    const f32 step_speed = std::min(agent.speed, dist / std::max(dt, 1e-4f));
    agent.velocity = to_corner * (step_speed / dist);
    agent.status = AgentStatus::kMoving;

    if (config.move) {
      transform.position[0] += agent.velocity.x * dt;
      transform.position[2] += agent.velocity.z * dt;
      f32 height;
      if (mesh.HeightAt(transform.position[0], transform.position[2], &height)) {
        transform.position[1] = height;
      }
      FaceVelocity(transform, agent.velocity);
    }
  });
}

}  // namespace rx::nav
