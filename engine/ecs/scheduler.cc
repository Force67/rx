#include "ecs/scheduler.h"

namespace rx::ecs {

void Scheduler::AddSystem(Stage stage, base::NameString name, SystemFn fn) {
  stages_[static_cast<size_t>(stage)].push_back({std::move(name), std::move(fn)});
}

void Scheduler::RunStage(Stage stage, World& world, f32 dt) {
  for (auto& system : stages_[static_cast<size_t>(stage)]) {
    system.fn(world, dt);
  }
}

}  // namespace rx::ecs
