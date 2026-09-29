#ifndef RX_APPS_SHELL_DEMOS_GRASS_H_
#define RX_APPS_SHELL_DEMOS_GRASS_H_

#include <base/containers/vector.h>

#include "rxe/ecs/world.h"
#include "rxe/render/core/renderer.h"
#include "rxe/render/geometry/procedural_grass.h"
#include "rxe/scene/fly_camera.h"
#include "viewer_config.h"

namespace rx::shell {

// Standalone procedural-grass showcase (--demo grass): rolling hills described
// by a compact semantic field, a growable placed-stone surface, coherent gusts,
// clumps, independent geometry/density LOD and a moving displacement source.
class GrassDemo {
 public:
  GrassDemo(const ViewerConfig* config, ecs::World* world, render::Renderer* renderer, scene::FlyCamera* camera, bool* scene_owns_sun) : config_(config), world_(world), renderer_(renderer), camera_(camera), scene_owns_sun_(scene_owns_sun) {}

  void Create();
  void Update(f32 dt);
  void Emit(render::FrameView& view);

 private:
  f32 TerrainHeight(f32 x, f32 z) const;
  void BuildField();
  void BuildTerrain();
  void BuildGrowableStone();

  const ViewerConfig* config_;
  ecs::World* world_;
  render::Renderer* renderer_;
  scene::FlyCamera* camera_;
  bool* scene_owns_sun_;
  base::Vector<render::GrassFieldSample> samples_;
  base::Vector<render::GrassType> types_;
  base::Vector<render::GrassSurfaceTriangle> surfaces_;
  render::GrassDomain domain_;
  ecs::Entity interaction_marker_{};
  Vec3 interaction_position_{};
  Vec3 interaction_direction_{};
  f32 time_ = 0.0f;
};

}  // namespace rx::shell

#endif  // RX_APPS_SHELL_DEMOS_GRASS_H_
