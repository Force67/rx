#ifndef RX_RUNTIME_DEMO_NAV_H_
#define RX_RUNTIME_DEMO_NAV_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "ecs/entity.h"
#include "engine_context.h"
#include "nav/agent.h"
#include "nav/navmesh.h"
#include "render/core/renderer.h"

namespace rx {

// Navigation acceptance demo (--demo nav): a cat-and-mouse chase across
// terrain that fights back. A porter wanders a procedural landscape -- rolling
// hills, a winding river, boulder fields, two steep unclimbable mesas -- while
// a pack of mules pursues it through rx::nav: cost-aware A* (rocks and water
// are walkable but expensive, with one-time entry tolls), per-frame funnel
// steering, and event-based repathing (the pack replans when the porter
// changes cells, never on a timer). Terrain desirability is painted into the
// vertex colors and the live navmesh/corridor overlay (RX_NAV_LINES=0 hides
// it).
class NavDemo {
 public:
  explicit NavDemo(EngineContext& ctx);

  // Builds the terrain mesh + navmesh, spawns the porter and the mule pack,
  // registers the chase system. Call once from DemoScenes::CreateDemoScene.
  void Create();

  // Emits the navmesh / corridor / steering debug lines into this frame's
  // view. Called from DemoScenes::EmitToView.
  void Emit(f32 dt, render::FrameView& view);

 private:
  // Ground truth for both the render mesh and the nav sampler.
  struct Ground {
    f32 height = 0;
    nav::AreaId area = nav::kAreaGround;
  };
  Ground SampleGround(f32 x, f32 z) const;

  void BuildTerrainMesh();
  void SpawnActors();

  EngineContext& ctx_;
  nav::NavMesh mesh_;

  ecs::Entity porter_{};
  base::Vector<ecs::Entity> mules_;
  u32 rng_ = 0x9e3779b9u;  // deterministic wander seed
  u32 steals_ = 0;         // cargo grabs so far (porter respawns)

  base::Vector<render::DebugLine> lines_;
  bool draw_lines_ = true;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_NAV_H_
