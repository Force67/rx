#ifndef RX_APPS_SHELL_DEMOS_NAV_H_
#define RX_APPS_SHELL_DEMOS_NAV_H_

#include <base/containers/vector.h>

#include "foundation/math/math.h"
#include "plugins/nav/agent.h"
#include "plugins/nav/navmesh.h"
#include "rxe/ecs/entity.h"
#include "rxe/ecs/scheduler.h"
#include "rxe/ecs/world.h"
#include "rxe/render/core/renderer.h"
#include "rxe/scene/fly_camera.h"
#include "viewer_config.h"

namespace rx::shell {

// Navigation acceptance demo (--demo nav): a cat-and-mouse chase across
// terrain that fights back. A porter wanders a procedural landscape (rolling
// hills, a winding river, boulder fields, two steep unclimbable mesas) while
// a pack of mules pursues it through rx::nav: cost-aware A* (rocks and water
// are walkable but expensive, with one-time entry tolls), per-frame funnel
// steering, and event-based repathing (the pack replans when the porter
// changes cells, never on a timer). Terrain desirability is painted into the
// vertex colors and the live navmesh/corridor overlay (RX_NAV_LINES=0 hides
// it).
class NavDemo {
 public:
  NavDemo(const ViewerConfig* config, ecs::World* world, ecs::Scheduler* scheduler, render::Renderer* renderer, scene::FlyCamera* camera);

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

  const ViewerConfig* config_;
  ecs::World* world_;
  ecs::Scheduler* scheduler_;
  render::Renderer* renderer_;
  scene::FlyCamera* camera_;
  nav::NavMesh mesh_;

  ecs::Entity porter_{};
  base::Vector<ecs::Entity> mules_;
  u32 rng_ = 0x9e3779b9u;  // deterministic wander seed
  u32 steals_ = 0;         // cargo grabs so far (porter respawns)

  base::Vector<render::DebugLine> lines_;
  bool draw_lines_ = true;
};

}  // namespace rx::shell

#endif  // RX_APPS_SHELL_DEMOS_NAV_H_
