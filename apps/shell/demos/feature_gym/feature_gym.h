#ifndef RX_APPS_SHELL_DEMOS_FEATURE_GYM_FEATURE_GYM_H_
#define RX_APPS_SHELL_DEMOS_FEATURE_GYM_FEATURE_GYM_H_

#include "base/memory/unique_pointer.h"
#include "base/strings/string_ref.h"
#include "apps/shell/viewer_config.h"
#include "base/containers/vector.h"
#include "foundation/build_config/types.h"

#if defined(__ANDROID__)
struct AAssetManager;
#endif

namespace rx {
namespace audio {
class AudioSystem;
}
namespace ecs {
class Scheduler;
class World;
}
namespace render {
struct FrameView;
class Renderer;
}
namespace physics {
class PhysicsWorld;
}
namespace scene {
class FlyCamera;
}
}  // namespace rx

namespace rx::shell {

#if defined(__ANDROID__)
void SetFeatureGymAssetManager(::AAssetManager* manager);
#endif

struct ViewerConfig;
class ShowcaseCamera;

// Self-contained RX acceptance world. This unit owns the layout and simulation
// policy for all feature districts; the application only dispatches creation,
// frame emission, and the optional deterministic camera tour.
class FeatureGym {
 public:
  FeatureGym(const ViewerConfig* config, ecs::World& world, ecs::Scheduler& scheduler,
             render::Renderer& renderer, physics::PhysicsWorld& physics, scene::FlyCamera* camera,
             audio::AudioSystem* audio, base::Vector<PhysicsEntity>* physics_entities, bool* scene_owns_sun);
  ~FeatureGym();

  FeatureGym(const FeatureGym&) = delete;
  FeatureGym& operator=(const FeatureGym&) = delete;

  void Create();
  void Emit(f32 dt, render::FrameView& view);

  // Adds every district, exhibit, and renderer-mode stop to `camera`. Returns
  // false when the gym was not created. SetTourTime applies the matching
  // deterministic simulation/weather/renderer state while the camera travels.
  bool BuildTour(ShowcaseCamera& camera);
  void SetTourTime(f32 seconds);

  base::StringRef active_area() const;

 private:
  struct Impl;
  base::UniquePointer<Impl> impl_;
};

}  // namespace rx::shell

#endif  // RX_APPS_SHELL_DEMOS_FEATURE_GYM_FEATURE_GYM_H_
