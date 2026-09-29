#ifndef RX_RUNTIME_VIEWER_INPUT_H_
#define RX_RUNTIME_VIEWER_INPUT_H_

#include "rxe/scene/fly_camera.h"
#include "rxe/ui/events/input_actions.h"

namespace rx::ui {
class InputMap;
}  // namespace rx::ui

namespace rx {


// The viewer's action set. The engine owns no verbs, so the application defines
// which actions and axes exist and registers their names + default bindings.
// The viewer only needs to fly the debug camera and toggle the debug overlay /
// physics toss, so this is deliberately small.
enum class Action : ui::ActionId {
  kMoveForward,
  kMoveBack,
  kMoveLeft,
  kMoveRight,
  kJump,
  kSprint,
  kSneak,
  kCamUp,       // free-fly camera rise
  kCamDown,     // free-fly camera descend
  kToggleDebug, // show/hide the debug overlay
  kThrowDebug,  // toss a physics cube
  kCount,
};

enum class Axis : ui::AxisId { kMoveX, kMoveY, kLookX, kLookY, kCount };

// Registers the viewer's action/axis names, digital->analog folds and default
// keyboard/mouse + gamepad bindings with `map`. Call once at startup.
void RegisterViewerInput(ui::InputMap& map);

// The free camera's intent from this frame's resolved viewer actions.
scene::FlyCameraInput FlyCameraIntent(const ui::ActionState& actions);

}  // namespace rx

#endif  // RX_RUNTIME_VIEWER_INPUT_H_
