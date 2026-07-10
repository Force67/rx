#ifndef RX_RUNTIME_VIEWER_INPUT_H_
#define RX_RUNTIME_VIEWER_INPUT_H_

#include "core/input_actions.h"

namespace rx {

class InputMap;

// The viewer's action set. The engine owns no verbs, so the application defines
// which actions and axes exist and registers their names + default bindings.
// The viewer only needs to fly the debug camera and toggle the debug overlay /
// physics toss, so this is deliberately small.
enum class Action : ActionId {
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

enum class Axis : AxisId { kMoveX, kMoveY, kLookX, kLookY, kCount };

// Registers the viewer's action/axis names, digital->analog folds and default
// keyboard/mouse + gamepad bindings with `map`. Call once at startup.
void RegisterViewerInput(InputMap& map);

}  // namespace rx

#endif  // RX_RUNTIME_VIEWER_INPUT_H_
