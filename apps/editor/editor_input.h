#ifndef RX_EDITOR_INPUT_H_
#define RX_EDITOR_INPUT_H_

#include "core/input_actions.h"

namespace rx {

class InputMap;

// The editor's action/axis set. The engine owns no verbs, so the app defines
// them. Movement + vertical fly for the free camera; sprint for fast travel.
enum class Action : ActionId {
  kMoveForward,
  kMoveBack,
  kMoveLeft,
  kMoveRight,
  kJump,
  kSprint,
  kSneak,
  kCamUp,
  kCamDown,
  kToggleDebug,
  kThrowDebug,
  kCount,
};

enum class Axis : AxisId { kMoveX, kMoveY, kLookX, kLookY, kCount };

void RegisterEditorInput(InputMap& map);

}  // namespace rx

#endif  // RX_EDITOR_INPUT_H_
