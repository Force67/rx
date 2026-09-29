#ifndef RX_APPS_SHELL_VIEWER_CONFIG_H_
#define RX_APPS_SHELL_VIEWER_CONFIG_H_


#include "base/strings/xstring.h"
#include "foundation/math/math.h"
#include "rxe/app/services.h"
#include "rxe/importers/usd/usd_loader.h"
#include "rxe/render/core/presets.h"
#include "rxe/render/core/renderer.h"

namespace rx::shell {

// The viewer's boot configuration: the app::AppConfig fields plus the
// front-door content selection (main.cc maps the overlap into AppConfig).
struct ViewerConfig {
  base::String scene_path;  // standalone gltf/glb or usd scene (e.g. sponza)
  base::String demo_scene;  // builtin demo scene id ("water", "materials", ...)
  // Prim paths whose authored `visibility` is overridden when loading a usd
  // stage. Scenes ship alternative configurations (day/night lighting rigs,
  // set dressing variants) toggled by visibility, and picking one is a
  // viewing decision, not an edit to the stage.
  importers::UsdLoadOptions usd_visibility;
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
  base::String shot_path;
  int shot_frames = 0;
  // --camera-at / --camera-look / --camera-fov: override the scene's own
  // viewpoint for this run without editing it. Looking at an authored scene
  // from a second angle is how an author finds the things one hero framing
  // hides - a facade that only works head-on, a silhouette that collapses from
  // the side, a street with nothing in its middle distance - and having to edit
  // the file to do it means the check costs an edit to undo, so it does not get
  // made. Empty leaves the scene's Camera alone; `camera_fov` 0 does the same.
  base::String camera_at;
  base::String camera_look;
  f32 camera_fov = 0.0f;
  // --world <archive.rxp>: mount a baked world and stream it around the camera
  // instead of loading a scene whole. `world_name` is the name it was cooked
  // under (rxworld --name), which is also the directory its index sits in.
  //
  // Empty takes the same default the cook does - world::WorldNameForArchive,
  // the archive's filename stem - so an archive from a bare `rxworld bake` or
  // the editor's Bake World opens with a bare `--world`. Only an archive baked
  // under a name that is not its filename, or one holding more than one world,
  // needs --world-name to say which.
  base::String world_path;
  base::String world_name;
  // --authoring-endpoint: serve the script commands on this unix socket path
  // while the engine runs, so a tool can spawn/move/inspect without a restart.
  // Empty (the default) means no endpoint is bound at all; see
  // authoring/command_bridge.h for why that is not merely a convenience.
  base::String authoring_socket;
};

// A dynamic physics body the host mirrors into an ECS transform after each
// step (the viewer's name for the host's binding type).
using PhysicsEntity = app::PhysicsBinding;

}  // namespace rx::shell

#endif  // RX_APPS_SHELL_VIEWER_CONFIG_H_
