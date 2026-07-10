#ifndef RX_ENGINE_APP_APPLICATION_H_
#define RX_ENGINE_APP_APPLICATION_H_

#include "app/services.h"
#include "render/core/presets.h"

namespace rx::app {

// What to boot: the renderer description, the hardware quality tier and
// whether to run headless. Content selection, CLI parsing and every other
// game decision stays with the application.
struct AppConfig {
  render::RendererDesc renderer;
  // kAuto picks a tier from the gpu at startup; the rest force one (steam
  // deck, android, low/medium/high/ultra, console).
  render::QualityPreset preset = render::QualityPreset::kAuto;
  bool headless = false;
  // When true (the default) the host gathers every visible
  // scene::Transform+scene::Renderable entity into the FrameView before
  // OnBuildView. A game that stores its renderables in its own component types,
  // or that needs a bespoke gather (skinned draws, decals, per-entity tint),
  // sets this false and appends every draw itself in OnBuildView.
  bool gather_entity_draws = true;
};

// The game's side of the host contract. Host::RunFrame drives these in order:
//
//   per fixed step:   ECS kPreSim/kSim/kPostSim stages, then OnFixedStep(dt)
//   every iteration:  OnSimulate(frame_delta)
//   per drawn frame:  OnUpdate, ECS kPreRender stage, host transform gather,
//                     OnBuildView, RenderFrame, OnFrameEnd
//
// A headless run ticks the fixed-step side and OnSimulate; the per-drawn-frame
// callbacks (OnUpdate/OnBuildView/OnFrameEnd) fire only when a window is up.
// All callbacks run on the main thread.
class Application {
 public:
  virtual ~Application() = default;

  // Subsystems are up and `services` is fully populated (renderer null when
  // headless). Load content, register ECS systems, spawn entities. Returning
  // false aborts startup; the host tears down what it brought up.
  virtual bool OnInitialize(Services& services) = 0;

  // Once per fixed simulation step, after the sim stages ran.
  virtual void OnFixedStep(f32 dt) { (void)dt; }

  // Once per host iteration, after the fixed-step loop, at frame cadence. Fires
  // in both windowed and headless runs, so authoritative game simulation that a
  // dedicated server must advance (scripting, networked actors, quest logic)
  // lives here rather than in the windowed-only OnUpdate.
  virtual void OnSimulate(f32 frame_delta) { (void)frame_delta; }

  // Once per drawn frame, before the kPreRender stage: read input, move the
  // camera, begin UI frames.
  virtual void OnUpdate(f32 frame_delta) { (void)frame_delta; }

  // The frame's view, already holding the host-gathered entity draws. Set
  // view.camera and append extra draws, lights, particles or UI callbacks.
  virtual void OnBuildView(f32 frame_delta, render::FrameView& view) {
    (void)frame_delta;
    (void)view;
  }

  // After the frame was submitted: capture hooks, benchmark bookkeeping, quit
  // decisions (Services::host->RequestQuit()).
  virtual void OnFrameEnd() {}

  // The renderer is idle but still alive: destroy GPU-dependent application
  // resources here (UI backends, uploaded meshes owned by the app).
  virtual void OnShutdown() {}
};

}  // namespace rx::app

#endif  // RX_ENGINE_APP_APPLICATION_H_
