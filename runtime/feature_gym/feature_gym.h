#ifndef RX_RUNTIME_FEATURE_GYM_FEATURE_GYM_H_
#define RX_RUNTIME_FEATURE_GYM_FEATURE_GYM_H_


#include "base/memory/unique_pointer.h"
#include "base/strings/string_ref.h"
#include "foundation/build_config/types.h"

#if defined(__ANDROID__)
struct AAssetManager;
#endif

namespace rx {

#if defined(__ANDROID__)
void SetFeatureGymAssetManager(::AAssetManager* manager);
#endif

struct EngineContext;
class ShowcaseCamera;

namespace render {
struct FrameView;
}

// Self-contained RX acceptance world. This unit owns the layout and simulation
// policy for all feature districts; the application only dispatches creation,
// frame emission, and the optional deterministic camera tour.
class FeatureGym {
 public:
  explicit FeatureGym(EngineContext& ctx);
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

}  // namespace rx

#endif  // RX_RUNTIME_FEATURE_GYM_FEATURE_GYM_H_
