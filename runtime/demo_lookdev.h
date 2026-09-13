#ifndef RX_RUNTIME_DEMO_LOOKDEV_H_
#define RX_RUNTIME_DEMO_LOOKDEV_H_

#include <memory>
#include <string>
#include <vector>

#include "asset/material.h"
#include "core/input.h"
#include "core/input_actions.h"
#include "core/math.h"
#include "engine_context.h"
#include "render/core/renderer.h"
#include "render/pipeline/human_material.h"

namespace rx {

// The character reference lab (`--demo lookdev`): a calibrated bench where a
// frozen head, camera and material are lit ONE LIGHT AT A TIME and compared
// against reference in the same colour path. Provides:
//   * an OLAT rig (front/side/grazing/back/top/bottom, both emitter extremes,
//     one multi-light stop);
//   * one nominal light direction routed through sun / sphere / rect panel, the
//     parity test for "every light type evaluates the same material";
//   * frozen camera presets (front, 30, three-quarter, profile, close-up,
//     gameplay distance, LOD distance);
//   * side-by-side / wipe / linear-difference / display-referred-difference
//     comparison with per-region masks;
//   * live global and per-region material editing with undo;
//   * coordinate-descent fitting against the measured error over all selected
//     OLAT frames at once;
//   * a deterministic capture pass over the full validation matrix.
//
// It owns its camera (the presets are the point), so the Viewer routes OnUpdate
// here and reads the pose back through Emit.
class LookdevDemo {
 public:
  explicit LookdevDemo(EngineContext& ctx);
  ~LookdevDemo();

  // Loads the subject (a glTF head from --gltf or assets/head, else a
  // procedural stand-in), assigns per-region character materials and builds the
  // OLAT rig. Call once from DemoScenes::CreateDemoScene.
  void Create();

  void Update(f32 dt, const InputState& input, const ActionState& actions, bool allow_keyboard,
              bool allow_mouse);

  // Writes the frozen camera, the active OLAT light and the panel.
  void Emit(f32 dt, render::FrameView& view);

  bool wants_mouse_capture() const { return false; }

  // True once a deterministic capture pass has written every frame of the
  // validation matrix; the Viewer quits on it under RX_LOOKDEV_QUIT.
  bool capture_finished() const;

  // the validation matrix
  // Named so the capture pass, the panel and the docs cannot drift apart.
  struct LightStop {
    const char* name;
    // Direction the light TRAVELS (engine sun convention). Normalized on use.
    Vec3 travel;
    enum class Kind : u8 { kNone, kSun, kSphere, kRect, kMulti } kind;
    f32 size;       // sphere radius / rect half-extent, metres
    f32 intensity;
  };
  struct CameraStop {
    const char* name;
    f32 yaw_degrees;    // 0 = straight on, 90 = profile
    f32 pitch_degrees;
    f32 distance;       // metres from the head centre
    f32 fov_degrees;
  };
  static std::span<const LightStop> light_stops();
  static std::span<const CameraStop> camera_stops();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_LOOKDEV_H_
