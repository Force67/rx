// rx::nav acceptance: costed A* (prefers smooth ground, commits after entry),
// capped/partial searches, funnel string-pulling, raycast shortcuts,
// event-based corridor validation and the delta-cost position query -- all on
// a synthetic sampler, no GPU, no physics.

#include "nav/agent.h"
#include "nav/navmesh.h"
#include "nav/path.h"
#include "nav/query.h"

#include <cmath>
#include <cstdio>

#include "ecs/world.h"
#include "scene/components.h"

namespace {

using namespace rx;
using namespace rx::nav;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "nav_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-3f) {
  if (std::fabs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "nav_test: FAIL: %s (got %.4f, expected %.4f)\n", message, actual,
               expected);
  ++failures;
}

constexpr AreaId kAreaRock = 2;
constexpr AreaId kAreaWater = 3;

// The synthetic world, 0..32m square:
//  - flat ground at y=0
//  - a rock band across x in [12, 20) for z < 16 (walkable but 4x cost +
//    entry toll); the z >= 16 half stays smooth ground
//  - a river along z in [24, 27): water, 3x cost
//  - a void hole (no surface) in x [4, 6), z [4, 6)
Sample WorldSample(f32 x, f32 z) {
  Sample s;
  s.height = 0;
  s.area = kAreaGround;
  if (x >= 12 && x < 20 && z < 16) s.area = kAreaRock;
  if (z >= 24 && z < 27) {
    s.area = kAreaWater;
    s.height = -0.3f;
  }
  if (x >= 4 && x < 6 && z >= 4 && z < 6) s.area = kAreaNone;
  return s;
}

NavMesh MakeWorld() {
  NavMesh mesh(NavMeshConfig{.cell_size = 0.5f, .tile_cells = 16});
  mesh.SetAreaCost(kAreaRock, 4.0f, 6.0f);
  mesh.SetAreaCost(kAreaWater, 3.0f, 1.0f);
  SampleFn sampler = [](f32 x, f32 z, Sample& out) {
    out = WorldSample(x, z);
    return out.area != kAreaNone;
  };
  // Cover the whole 32m square (tiles are 8m at 0.5m cells).
  while (mesh.EnsureBubble({16, 0, 16}, 24.0f, sampler, 64) > 0) {
  }
  return mesh;
}

bool CorridorTouchesArea(const NavMesh& mesh, const Corridor& corridor, AreaId area) {
  for (const CellRef& cell : corridor.cells) {
    if (mesh.Area(cell) == area) return true;
  }
  return false;
}

void TestBuildAndQueries(NavMesh& mesh) {
  Check(mesh.tile_count() >= 16, "bubble built the full grid of tiles");
  Check(mesh.Walkable(mesh.CellAt({1, 0, 1})), "open ground walkable");
  Check(!mesh.Walkable(mesh.CellAt({5, 0, 5})), "hole is not walkable");
  Check(mesh.Area(mesh.CellAt({14, 0, 8})) == kAreaRock, "rock band painted by sampler");
  Check(mesh.Area(mesh.CellAt({16, 0, 25})) == kAreaWater, "river present");
  f32 h = 1;
  Check(mesh.HeightAt(10, 10, &h) && std::fabs(h) < 1e-3f, "surface height on open ground");

  const CellRef clamp = mesh.ClampToWalkable({5.0f, 0, 5.0f}, 3.0f);
  Check(clamp.valid() && mesh.Walkable(clamp), "off-mesh point clamps to walkable cell");

  const NavRaycast clear = mesh.Raycast({1, 0, 1}, {3, 0, 3});
  Check(clear.clear, "raycast over open ground is clear");
  Near(clear.cost, clear.length, "unit-cost ground: weighted == raw");
  const NavRaycast blocked = mesh.Raycast({3, 0, 5}, {8, 0, 5});
  Check(!blocked.clear, "raycast through the hole is blocked");
}

void TestCostedAstar(NavMesh& mesh) {
  PathScratch scratch;
  Corridor corridor;

  // Start west of the rock band, goal east of it, both at z=8 (the band spans
  // z<16, detour available at z>=16). The weighted route must leave the
  // straight line and skirt the rocks.
  PathRequest request;
  request.start = {8, 0, 4};
  request.goal = {24, 0, 4};
  request.max_iterations = 4000;
  Check(FindPath(mesh, request, scratch, &corridor) == PathStatus::kComplete,
        "path across the map completes");
  Check(!CorridorTouchesArea(mesh, corridor, kAreaRock),
        "expensive rock band is detoured around");

  // Make rock cheap: now the straight line through it must win.
  mesh.SetAreaCost(kAreaRock, 1.0f, 0.0f);
  Check(FindPath(mesh, request, scratch, &corridor) == PathStatus::kComplete,
        "path with cheap rocks completes");
  Check(CorridorTouchesArea(mesh, corridor, kAreaRock), "cheap rocks are crossed directly");
  mesh.SetAreaCost(kAreaRock, 4.0f, 6.0f);

  // Iteration cap: a tiny budget cannot reach a far goal but must still make
  // forward progress as a partial path.
  request.max_iterations = 40;
  const PathStatus capped = FindPath(mesh, request, scratch, &corridor);
  Check(capped == PathStatus::kPartial, "tiny iteration budget yields a partial path");
  Check(corridor.cells.size() > 1, "partial path still leaves the start cell");
  const Vec3 end = mesh.CellCenter(corridor.cells.back());
  Check(end.x > request.start.x + 0.5f, "partial path advances toward the goal");

  // Unreachable goal (inside the hole, clamp radius too small to escape it
  // sideways): the search must clamp to the nearest walkable cell and finish.
  request.max_iterations = 4000;
  request.goal = {5.0f, 0, 5.0f};
  Check(FindPath(mesh, request, scratch, &corridor) == PathStatus::kComplete,
        "goal in a hole resolves to its clamped rim cell");
  Check(mesh.Walkable(corridor.cells.back()), "clamped corridor ends on walkable ground");
}

void TestEntryCostCommitment(NavMesh& mesh) {
  // The Death Stranding backtrack fix: entering rock costs 6m once, walking
  // inside costs 4x. An agent OUTSIDE avoids the band; an agent two steps
  // INSIDE it does not pay the toll again, so short hops within the band must
  // not detour all the way back out first.
  PathScratch scratch;
  Corridor corridor;

  // From inside the band to a point 2m ahead, still inside.
  PathRequest inside;
  inside.start = {14, 0, 8};
  inside.goal = {17, 0, 8};
  inside.max_iterations = 4000;
  Check(FindPath(mesh, inside, scratch, &corridor) == PathStatus::kComplete,
        "short hop inside the rock band completes");
  Check(CorridorTouchesArea(mesh, corridor, kAreaRock), "hop stays committed to the rocks");
  // A detour out of the band and back would pay entry again + a long walk:
  // the direct hop is ~3m * 4x = 12 weighted meters; going around is > 20.
  Check(corridor.cost < 16.0f, "committed hop is priced as in-area travel only");

  // Cost audit: step into the band pays entry exactly once.
  const CellRef out = mesh.CellAt({11.7f, 0, 8});
  const CellRef in = mesh.CellAt({12.2f, 0, 8});
  const f32 step = mesh.StepCost(out, in);
  Near(step, 0.5f * 0.5f * (1.0f + 4.0f) + 6.0f, "entry step = blended traverse + toll");
  const f32 within = mesh.StepCost(in, mesh.CellAt({12.7f, 0, 8}));
  Near(within, 0.5f * 4.0f, "in-area step pays no toll");
}

void TestFunnelAndShortcut(NavMesh& mesh) {
  PathScratch scratch;
  Corridor corridor;
  PathRequest request;
  request.start = {1.25f, 0, 30.25f};  // cell centers, the realistic case
  request.goal = {13.75f, 0, 30.25f};
  request.max_iterations = 4000;
  Check(FindPath(mesh, request, scratch, &corridor) == PathStatus::kComplete,
        "open-field path completes");

  // Open field: the funnel must collapse the corridor to (almost) a straight
  // shot -- the goal and maybe one corner, not a zig-zag of cell midpoints.
  base::Vector<Vec3> corners;
  FunnelCorners(mesh, corridor, request.start, 0, 0.2f, 16, &corners);
  Check(corners.size() <= 2, "funnel over open field yields at most one bend");
  const Vec3 last = corners.back();
  Near(last.x, 13.75f, "funnel ends at the goal x", 0.6f);
  Near(last.z, 30.25f, "funnel ends at the goal z", 0.6f);

  // NextCorner from the live position steers at that same target.
  Vec3 corner;
  Check(NextCorner(mesh, &corridor, request.start, 0.2f, &corner),
        "agent on corridor gets a corner");
  Check(corner.x > 1.3f, "corner is ahead of the agent");
}

void TestEventRepathing(NavMesh& mesh) {
  PathScratch scratch;
  Corridor corridor;
  PathRequest request;
  request.start = {2, 0, 20};
  request.goal = {10, 0, 20};
  request.max_iterations = 4000;
  Check(FindPath(mesh, request, scratch, &corridor) == PathStatus::kComplete, "baseline path");

  // Valid corridor, nothing moved: no repath event.
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 20}, request.goal) == RepathReason::kNone,
        "fresh corridor validates clean");

  // Goal drifts WITHIN its cell: still no event (the funnel tracks it).
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 20}, {10.1f, 0, 20.1f}) == RepathReason::kNone,
        "sub-cell goal drift does not repath");

  // Goal jumps to another walkable cell: event.
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 20}, {10, 0, 22}) ==
            RepathReason::kGoalCellChanged,
        "goal cell change triggers repath");

  // Agent teleported off the corridor: event.
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 30}, request.goal) ==
            RepathReason::kLeftCorridor,
        "leaving the corridor triggers repath");

  // Paint a blocker's cost across the corridor: the touched tile's version
  // bumps and the corridor notices.
  FindPath(mesh, request, scratch, &corridor);
  mesh.PaintDisc({6, 0, 20}, 1.0f, kAreaRock);
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 20}, request.goal) ==
            RepathReason::kTileInvalidated,
        "painting under the corridor triggers repath");

  // Off-mesh goal whose clamp moves: event. Goal sits in the hole; corridor
  // clamps to its rim. A new goal deep on the other side of the hole clamps
  // to a different rim cell.
  PathRequest hole;
  hole.start = {2, 0, 5};
  hole.goal = {4.2f, 0, 5.0f};
  hole.max_iterations = 4000;
  FindPath(mesh, hole, scratch, &corridor);
  Check(corridor.status == PathStatus::kComplete, "hole-rim path completes");
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 5}, {5.8f, 0, 5.0f}) ==
            RepathReason::kClampedGoalChanged,
        "clamped destination change triggers repath");

  // Partial path: reaching its end must request an extension.
  PathRequest capped;
  capped.start = {8, 0, 4};
  capped.goal = {24, 0, 4};
  capped.max_iterations = 40;
  Check(FindPath(mesh, capped, scratch, &corridor) == PathStatus::kPartial, "capped partial");
  const Vec3 end = mesh.CellCenter(corridor.cells.back());
  Check(ValidateCorridor(mesh, &corridor, end, capped.goal) == RepathReason::kEndOfPartialPath,
        "end of a partial path requests the next stretch");
}

void TestPositionQuery(NavMesh& mesh) {
  // Two candidates equidistant from an agent south of the river: one on the
  // same bank, one across the water. The delta-cost score must pick the same
  // bank.
  PositionQueryParams params;
  params.origin = {16, 0, 21};
  params.max_cost = 200;
  params.max_iterations = 20000;
  PositionCandidate candidates[2];
  candidates[0].position = {23, 0, 21};  // same bank
  candidates[1].position = {16, 0, 29};  // across the river
  const size_t best = EvaluatePositions(mesh, params, candidates);
  Check(best == 0, "same-bank position wins over crossing the river");
  Check(candidates[0].reachable && candidates[1].reachable, "both candidates reachable");
  Check(candidates[1].delta_cost > candidates[0].delta_cost + 1.0f,
        "crossing the river carries the delta cost");

  // Agent already IN the river: normalization must not freeze it in place --
  // the nearest bank must beat staying put even though every move costs.
  PositionQueryParams wet;
  wet.origin = {16, -0.3f, 25.5f};
  wet.max_cost = 200;
  wet.max_iterations = 20000;
  PositionCandidate escape[2];
  escape[0].position = {16, -0.3f, 25.6f};  // stay in the water
  escape[1].position = {16, 0, 22.5f};      // nearest dry bank
  const size_t out = EvaluatePositions(mesh, wet, escape);
  Check(out == 1, "agent in the river prefers the near bank over standing still");
}

void TestEcsAgents(NavMesh& mesh) {
  // Headless sim: an agent walks to its goal through the ECS system alone.
  ecs::World world;
  ecs::Entity entity = world.Create();
  world.Add(entity, scene::Transform{.position = {2, 0, 20}});
  NavAgent agent;
  agent.goal = {10, 0, 20};
  agent.speed = 4.0f;
  agent.active = true;
  world.Add(entity, agent);

  AgentUpdateConfig config;
  const f32 dt = 1.0f / 60.0f;
  int ticks = 0;
  for (; ticks < 60 * 20; ++ticks) {
    UpdateAgents(world, mesh, config, dt);
    if (!world.Get<NavAgent>(entity)->active) break;
  }
  const scene::Transform* t = world.Get<scene::Transform>(entity);
  Check(world.Get<NavAgent>(entity)->status == AgentStatus::kIdle, "agent settled to idle");
  Near(t->position[0], 10.0f, "agent arrived at goal x", 0.6f);
  Near(t->position[2], 20.0f, "agent arrived at goal z", 0.6f);
  Check(ticks < 60 * 6, "8m walk at 4m/s lands well inside 6 sim-seconds");
  Check(world.Get<NavAgent>(entity)->repath_count <= 3,
        "steady walk repaths a handful of times, not per tick");
}

void TestOffMeshAgentRecovers(NavMesh& mesh) {
  // Agent standing in the middle of the hole, more than one cell from any
  // walkable ground: the plan starts from a clamped cell it is not on. It
  // must walk back onto the corridor and arrive -- not freeze in kWaiting
  // while burning a replan every tick on a corridor it cannot attach to.
  ecs::World world;
  ecs::Entity entity = world.Create();
  world.Add(entity, scene::Transform{.position = {5, 0, 5}});
  NavAgent agent;
  agent.goal = {10, 0, 5};
  agent.speed = 4.0f;
  agent.active = true;
  world.Add(entity, agent);

  AgentUpdateConfig config;
  const f32 dt = 1.0f / 60.0f;
  int ticks = 0;
  for (; ticks < 60 * 20; ++ticks) {
    UpdateAgents(world, mesh, config, dt);
    if (!world.Get<NavAgent>(entity)->active) break;
  }
  const scene::Transform* t = world.Get<scene::Transform>(entity);
  Check(world.Get<NavAgent>(entity)->status == AgentStatus::kIdle,
        "off-mesh agent reached its goal");
  Near(t->position[0], 10.0f, "off-mesh agent arrived at goal x", 0.6f);
  Near(t->position[2], 5.0f, "off-mesh agent arrived at goal z", 0.6f);
  Check(world.Get<NavAgent>(entity)->repath_count <= 3,
        "approach does not replan every tick");
}

void TestTileRemoval(NavMesh& mesh) {
  PathScratch scratch;
  Corridor corridor;
  PathRequest request;
  request.start = {2, 0, 20};
  request.goal = {10, 0, 20};
  request.max_iterations = 4000;
  Check(FindPath(mesh, request, scratch, &corridor) == PathStatus::kComplete,
        "path before tile removal completes");

  // Drop everything but the tiles right around the start: a corridor tile
  // now reads version 0 and validation must notice.
  const u32 before = mesh.tile_count();
  const u32 dropped = mesh.RemoveTilesBeyond({2, 0, 20}, 4.0f);
  Check(dropped > 0 && mesh.tile_count() < before, "far tiles were dropped");
  Check(ValidateCorridor(mesh, &corridor, {2, 0, 20}, request.goal) ==
            RepathReason::kTileInvalidated,
        "corridor over a dropped tile triggers repath");
}

}  // namespace

int main() {
  // Fresh world per test: several tests mutate the mesh (area costs, paint,
  // tile removal) and must not leak state into each other.
  {
    NavMesh mesh = MakeWorld();
    TestBuildAndQueries(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestCostedAstar(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestEntryCostCommitment(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestFunnelAndShortcut(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestEventRepathing(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestPositionQuery(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestEcsAgents(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestOffMeshAgentRecovers(mesh);
  }
  {
    NavMesh mesh = MakeWorld();
    TestTileRemoval(mesh);
  }
  if (failures == 0) std::printf("nav_test: all checks passed\n");
  return failures;
}
