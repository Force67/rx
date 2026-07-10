#ifndef RX_ECS_SCHEDULER_H_
#define RX_ECS_SCHEDULER_H_

#include <base/containers/static_function.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"
#include "ecs/world.h"

namespace rx::ecs {

// Systems are plain functions. No base classes, no virtual dispatch.
// Closures are stored inline; captures must fit and be copy-constructible.
using SystemFn = base::StaticFunction<void(World& world, f32 dt), 256>;

enum class Stage { kPreSim, kSim, kPostSim, kPreRender, kStageCount };

class RX_ECS_EXPORT Scheduler {
 public:
  void AddSystem(Stage stage, base::NameString name, SystemFn fn);
  void RunStage(Stage stage, World& world, f32 dt);

 private:
  struct System {
    base::NameString name;
    SystemFn fn;
  };

  base::Vector<System> stages_[static_cast<size_t>(Stage::kStageCount)];
};

}  // namespace rx::ecs

#endif  // RX_ECS_SCHEDULER_H_
