#ifndef RX_ENGINE_APP_SERVICES_H_
#define RX_ENGINE_APP_SERVICES_H_

#include <base/containers/vector.h>

#include "asset/vfs.h"
#include "audio/audio_system.h"
#include "core/input_bindings.h"
#include "core/job_system.h"
#include "core/window.h"
#include "core/world_clock.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "physics/physics_world.h"
#include "render/core/renderer.h"

namespace rx::app {

class Host;

// A dynamic physics body the host mirrors into an ECS transform after each
// fixed step. Applications append to Services::physics_bindings to enroll a
// body; destroyed entities are simply skipped.
struct PhysicsBinding {
  physics::BodyId body;
  ecs::Entity entity;
};

// A physics strand groom the host mirrors into a renderer hair groom every
// frame: simulated node positions are read back and fed to the ribbon draw.
// Applications append to Services::hair_bindings; dead handles are skipped.
struct HairStrandBinding {
  physics::StrandGroomId strands;
  u32 groom;  // renderer hair groom id
};

// The engine services the host owns, handed to the application at
// OnInitialize. Addresses are stable for the host's lifetime.
struct Services {
  Host* host = nullptr;  // RequestQuit / surface lifecycle

  Window* window = nullptr;  // null when headless
  JobSystem* jobs = nullptr;
  WorldClock* clock = nullptr;  // advanced by the host each frame

  ecs::World* world = nullptr;
  ecs::Scheduler* scheduler = nullptr;

  // Always present, but uninitialized in headless runs: safe to hand to
  // subsystems that hold a reference, not to submit through.
  render::Renderer* renderer = nullptr;
  physics::PhysicsWorld* physics = nullptr;

  asset::Vfs* vfs = nullptr;
  audio::AudioSystem* audio = nullptr;

  // Bindings + the per-frame resolved action snapshot. The host resolves them
  // each pump; the application may rebind (InputMap is serialisable).
  InputMap* input_map = nullptr;
  const ActionState* actions = nullptr;

  base::Vector<PhysicsBinding>* physics_bindings = nullptr;
  base::Vector<HairStrandBinding>* hair_bindings = nullptr;
};

}  // namespace rx::app

#endif  // RX_ENGINE_APP_SERVICES_H_
