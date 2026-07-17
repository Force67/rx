#ifndef RX_CORE_INPUT_H_
#define RX_CORE_INPUT_H_

#include "core/types.h"

namespace rx {

// The physical keys the engine can deliver to bindings. Purely device codes;
// which action each drives is a game decision (see InputMap). Backends
// translate their native codes, unknown keys drop. Append only: the numeric
// codes are frozen so a game's C# key bridge stays stable across versions.
enum class Key : u8 {
  kW,
  kA,
  kS,
  kD,
  kQ,
  kE,
  kF,
  kT,
  kC,
  kR,
  kG,
  kX,
  kZ,
  kB,
  kV,
  kSpace,
  kLeftShift,
  kLeftCtrl,
  kEscape,
  kF1,
  kF2,
  kF3,
  kF4,
  kF5,
  kDelete,
  kBackspace,  // text fields: erase
  kReturn,     // text fields: commit
  k1,
  k2,
  k3,
  k4,
  kJ,
  // The keys below were appended after the original set; keep the prior codes
  // stable for any C# KeyPressed bridge that mirrors this enum.
  kArrowUp,
  kArrowDown,
  kArrowLeft,
  kArrowRight,
  kTab,
  kM,
  k5,
  k6,
  kCount,
};

enum class MouseButton : u8 { kLeft, kRight, kMiddle, kCount };

// Gamepad buttons, named by position (SDL's modern convention) so PS5 and Xbox
// pads map the same way: kSouth is Cross/A, kEast is Circle/B, etc. Backends
// translate their native codes; unknown buttons drop.
enum class GamepadButton : u8 {
  kSouth,   // A / Cross
  kEast,    // B / Circle
  kWest,    // X / Square
  kNorth,   // Y / Triangle
  kBack,    // View / Share
  kGuide,   // Xbox / PS button
  kStart,   // Menu / Options
  kLeftStick,
  kRightStick,
  kLeftShoulder,
  kRightShoulder,
  kDpadUp,
  kDpadDown,
  kDpadLeft,
  kDpadRight,
  kTouchpad,  // DualSense touchpad click (absent on Xbox)
  kCount,
};

enum class GamepadAxis : u8 {
  kLeftX,
  kLeftY,
  kRightX,
  kRightY,
  kLeftTrigger,
  kRightTrigger,
  kCount,
};

// Polled gamepad state, filled alongside InputState during PumpEvents. Sticks
// are [-1,1] (down/right positive); triggers are [0,1]. Kept separate from
// InputState so the keyboard/mouse path and the C# key bridge stay untouched.
struct GamepadState {
  enum class Kind : u8 { kUnknown, kXbox, kDualSense };
  bool connected = false;
  Kind kind = Kind::kUnknown;
  bool buttons[static_cast<u8>(GamepadButton::kCount)] = {};
  bool pressed[static_cast<u8>(GamepadButton::kCount)] = {};  // went down this pump
  f32 axes[static_cast<u8>(GamepadAxis::kCount)] = {};

  bool button(GamepadButton b) const { return buttons[static_cast<u8>(b)]; }
  bool button_pressed(GamepadButton b) const { return pressed[static_cast<u8>(b)]; }
  f32 axis(GamepadAxis a) const { return axes[static_cast<u8>(a)]; }
};

// DualSense adaptive-trigger request. A no-op on pads that lack the feature
// (Xbox), so callers can issue it unconditionally.
struct TriggerEffect {
  enum class Type : u8 { kOff, kResistance, kWeapon, kVibration };
  Type type = Type::kOff;
  u8 start = 0;     // 0-255: position along the pull where the effect begins
  u8 strength = 0;  // 0-255: resistance / vibration amplitude
};

// Polled state the window backend fills during PumpEvents. Deltas cover one
// pump and reset on the next.
struct InputState {
  bool keys[static_cast<u8>(Key::kCount)] = {};
  bool pressed[static_cast<u8>(Key::kCount)] = {};  // went down this pump
  bool mouse[static_cast<u8>(MouseButton::kCount)] = {};
  f32 mouse_dx = 0;
  f32 mouse_dy = 0;
  // Absolute cursor position in window pixels, persisted across pumps. Used by
  // the gui (the camera uses the deltas above).
  f32 mouse_x = 0;
  f32 mouse_y = 0;
  f32 wheel = 0;
  // UTF-8 text typed this pump (resets each PumpEvents, like the deltas). Filled
  // from the platform's text-input events so editor text fields (the asset
  // search box) can read characters without the engine binding every key.
  char text[32] = {};
  u8 text_len = 0;

  bool key(Key k) const { return keys[static_cast<u8>(k)]; }
  bool key_pressed(Key k) const { return pressed[static_cast<u8>(k)]; }
  bool button(MouseButton b) const { return mouse[static_cast<u8>(b)]; }
};

}  // namespace rx

#endif  // RX_CORE_INPUT_H_
