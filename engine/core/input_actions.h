#ifndef RX_CORE_INPUT_ACTIONS_H_
#define RX_CORE_INPUT_ACTIONS_H_

#include "core/types.h"

namespace rx {

// Semantic, device-agnostic input. Gameplay queries resolved *actions* and
// *axes* instead of raw keys, so the same code serves keyboard/mouse and
// gamepad and bindings can be remapped at runtime (see InputMap in
// input_bindings.h). Raw InputState / GamepadState stay available for text
// fields, the editor, and the C# bridge.
//
// The engine owns none of the actions: which verbs and screens exist is a game
// decision. An application defines its own action/axis enums, registers their
// stable names + default bindings with an InputMap at startup, and queries the
// resolved ActionState with those enum values. Action / axis ids are plain
// small integers here; the game's enums cast to them.
using ActionId = u16;
using AxisId = u16;

// Fixed upper bounds on how many actions/axes an application may register. Sized
// generously; the resolved ActionState carries one slot per possible id so the
// snapshot stays a trivially-copyable POD regardless of the game's set.
inline constexpr int kMaxActions = 64;
inline constexpr int kMaxAxes = 8;

enum class InputDevice : u8 { kKeyboardMouse, kGamepad };

// Resolved per pump from InputState + GamepadState through an InputMap. Indexed
// by the application's action/axis ids (its enum values cast to an integer).
struct ActionState {
  bool held[kMaxActions] = {};
  bool edge[kMaxActions] = {};  // went down this pump
  f32 analog[kMaxAxes] = {};
  // Which device produced the most recent input, for prompt glyphs.
  InputDevice last_device = InputDevice::kKeyboardMouse;

  // Accept the game's own enum (or a raw id) directly, so call sites read
  // actions.down(Action::kJump) without casting.
  template <class A>
  bool down(A a) const {
    return held[static_cast<int>(a)];
  }
  template <class A>
  bool pressed(A a) const {
    return edge[static_cast<int>(a)];
  }
  template <class X>
  f32 axis(X x) const {
    return analog[static_cast<int>(x)];
  }
};

}  // namespace rx

#endif  // RX_CORE_INPUT_ACTIONS_H_
