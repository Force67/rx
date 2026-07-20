#ifndef RX_RUNTIME_DEMO_GRASS_H_
#define RX_RUNTIME_DEMO_GRASS_H_

#include <base/containers/vector.h>

#include "engine_context.h"
#include "render/geometry/procedural_grass.h"

namespace rx {

// Standalone procedural-grass showcase (--demo grass): rolling hills described
// by a compact semantic field, a growable placed-stone surface, coherent gusts,
// clumps, independent geometry/density LOD and a moving displacement source.
class GrassDemo {
 public:
  explicit GrassDemo(EngineContext& ctx) : ctx_(ctx) {}

  void Create();
  void Update(f32 dt);
  void Emit(render::FrameView& view);

 private:
  f32 TerrainHeight(f32 x, f32 z) const;
  void BuildField();
  void BuildTerrain();
  void BuildGrowableStone();

  EngineContext& ctx_;
  base::Vector<render::GrassFieldSample> samples_;
  base::Vector<render::GrassType> types_;
  base::Vector<render::GrassSurfaceTriangle> surfaces_;
  render::GrassDomain domain_;
  ecs::Entity interaction_marker_{};
  Vec3 interaction_position_{};
  Vec3 interaction_direction_{};
  f32 time_ = 0.0f;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_GRASS_H_
