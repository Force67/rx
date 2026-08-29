#ifndef RX_RUNTIME_ENGINE_CONTEXT_H_
#define RX_RUNTIME_ENGINE_CONTEXT_H_

#include <string>

#include <base/containers/vector.h>

#include "app/services.h"
#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "audio/audio_system.h"
#include "core/input_actions.h"
#include "core/math.h"
#include "debug_ui.h"
#include "ecs/scheduler.h"
#include "ecs/world.h"
#include "fly_camera.h"
#include "physics/physics_world.h"
#include "render/core/presets.h"
#include "render/core/renderer.h"
#include "asset/usd_loader.h"

namespace rx {

// The viewer's boot configuration: the app::AppConfig fields plus the
// front-door content selection (main.cc maps the overlap into AppConfig).
struct EngineConfig {
  std::string scene_path;  // standalone gltf/glb or usd scene (e.g. sponza)
  std::string demo_scene;  // builtin demo scene id ("water", "materials", ...)
  // Prim paths whose authored `visibility` is overridden when loading a usd
  // stage. Scenes ship alternative configurations (day/night lighting rigs,
  // set dressing variants) toggled by visibility, and picking one is a
  // viewing decision, not an edit to the stage.
  asset::UsdLoadOptions usd_visibility;
  render::RendererDesc renderer;
  // Hardware quality tier. kAuto picks one from the gpu at startup; the rest
  // force a tier (steam deck, android, low/medium/high/ultra, console).
  render::QualityPreset preset = render::QualityPreset::kAuto;
  // No renderer at all: content skips its GPU uploads and the host runs the
  // simulation side only. Not the same as "no window" - see `offscreen`.
  bool headless = false;
  // No window, but a windowless renderer drawing into an offscreen image so a
  // display-less run can still capture a png. Content uploads exactly as it
  // does windowed, so `headless` is false whenever this is set.
  bool offscreen = false;
  // --shot: write the frame after `shot_frames` as a png and quit. Empty falls
  // back to the RX_UI_SHOT env var (viewer.cc), which existing capture scripts
  // drive; 0 frames falls back to RX_UI_SHOT_FRAMES.
  std::string shot_path;
  int shot_frames = 0;
  // --camera-at / --camera-look / --camera-fov: override the scene's own
  // viewpoint for this run without editing it. Looking at an authored scene
  // from a second angle is how an author finds the things one hero framing
  // hides - a facade that only works head-on, a silhouette that collapses from
  // the side, a street with nothing in its middle distance - and having to edit
  // the file to do it means the check costs an edit to undo, so it does not get
  // made. Empty leaves the scene's Camera alone; `camera_fov` 0 does the same.
  std::string camera_at;
  std::string camera_look;
  f32 camera_fov = 0.0f;
  // --world <archive.rxp>: mount a baked world and stream it around the camera
  // instead of loading a scene whole. `world_name` is the name it was cooked
  // under (rxworld --name), which is also the directory its index sits in.
  std::string world_path;
  std::string world_name = "streamworld";
  // --authoring-endpoint: serve the script commands on this unix socket path
  // while the engine runs, so a tool can spawn/move/inspect without a restart.
  // Empty (the default) means no endpoint is bound at all; see
  // authoring/command_bridge.h for why that is not merely a convenience.
  std::string authoring_socket;
};

// A dynamic physics body the host mirrors into an ECS transform after each
// step (the viewer's name for the host's binding type).
using PhysicsEntity = app::PhysicsBinding;

// Shared services the viewer subsystems (demo scenes, debug overlay) read
// through: the host-owned engine services plus the viewer's own camera, UI
// and config. The viewer populates it at OnInitialize.
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
  base::Vector<app::HairStrandBinding>* hair_bindings = nullptr;
  // Resolved semantic input this frame (move/look axes); null before the first
  // pump. Demos read it to drive interactive behaviour (e.g. locomotion speed).
  const ActionState* actions = nullptr;

  // Late-built services, null until the engine creates them.
  asset::AssetDatabase* assets = nullptr;

  // Demo scenes that stage their own lighting set this so the day/night clock
  // stops re-driving sun direction/intensity/ambient every frame (RX_SUN_DIR
  // has the same effect globally).
  bool scene_owns_sun = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_ENGINE_CONTEXT_H_
