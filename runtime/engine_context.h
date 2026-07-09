#ifndef RX_RUNTIME_ENGINE_CONTEXT_H_
#define RX_RUNTIME_ENGINE_CONTEXT_H_

#include <string>

#include <base/containers/vector.h>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "audio/audio_system.h"
#include "core/math.h"
#include "debug_ui.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "fly_camera.h"
#include "physics/physics_world.h"
#include "render/core/presets.h"
#include "render/core/renderer.h"

namespace rx {

struct EngineConfig {
  std::string gltf_path;   // standalone gltf/glb scene (e.g. sponza)
  std::string demo_scene;  // builtin demo scene id ("water", "materials", ...)
  render::RendererDesc renderer;
  // Hardware quality tier. kAuto picks one from the gpu at startup; the rest
  // force a tier (steam deck, android, low/medium/high/ultra, console).
  render::QualityPreset preset = render::QualityPreset::kAuto;
  bool headless = false;
};

// A dynamic physics body mirrored into an ECS transform after each step.
struct PhysicsEntity {
  physics::BodyId body;
  ecs::Entity entity;
};

// Shared services the engine subsystems read through. The engine owns this and
// the systems it created; pointers to late-built services (assets) are filled
// in once those exist.
struct EngineContext {
  const EngineConfig* config = nullptr;

  // Always-present services (engine members; addresses stable for its lifetime).
  ecs::World* world = nullptr;
  ecs::Scheduler* scheduler = nullptr;
  render::Renderer* renderer = nullptr;
  FlyCamera* camera = nullptr;
  physics::PhysicsWorld* physics = nullptr;
  asset::Vfs* vfs = nullptr;
  audio::AudioSystem* audio = nullptr;
  DebugUi* debug_ui = nullptr;
  base::Vector<PhysicsEntity>* physics_entities = nullptr;

  // Late-built services, null until the engine creates them.
  asset::AssetDatabase* assets = nullptr;

  // Demo scenes that stage their own lighting set this so the day/night clock
  // stops re-driving sun direction/intensity/ambient every frame (RX_SUN_DIR
  // has the same effect globally).
  bool scene_owns_sun = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_ENGINE_CONTEXT_H_
