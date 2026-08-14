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
  kL,
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

// One active contact on a touch screen. Positions are window pixels, matching
// InputState's mouse_x/mouse_y (SDL reports fingers normalized; the backend
// scales them), and deltas cover one pump.
struct TouchPoint {
  i64 id = -1;  // stable while the finger stays down, reused after it lifts
  f32 x = 0;
  f32 y = 0;
  f32 dx = 0;
  f32 dy = 0;
  f32 pressure = 0;       // 0..1, panels without pressure report 1 while down
  bool pressed = false;   // went down this pump
  bool released = false;  // lifted this pump; the slot is gone next pump
};

// Polled touch-screen state, filled alongside InputState during PumpEvents.
// Kept separate from InputState for the same reason GamepadState is: the
// keyboard/mouse path and its C# key bridge stay untouched.
//
// A finger that lifts stays visible for exactly one pump with released set, so
// a frame that only samples state still sees the tap end.
struct TouchState {
  static constexpr u32 kMaxPoints = 10;  // the Deck's panel reports ten

  // What a contact update does. Backends translate their native events into
  // these; a canceled gesture (the compositor claimed it) reports kUp, so
  // consumers only ever handle one end state.
  enum class Phase : u8 { kDown, kMove, kUp };

  TouchPoint points[kMaxPoints] = {};
  u32 count = 0;

  const TouchPoint* Find(i64 id) const {
    for (u32 i = 0; i < count; ++i) {
      if (points[i].id == id) return &points[i];
    }
    return nullptr;
  }

  // True while any finger is on the panel (released ones no longer count).
  bool active() const {
    for (u32 i = 0; i < count; ++i) {
      if (!points[i].released) return true;
    }
    return false;
  }

  // Contacts still down, which is what a gesture decision cares about.
  u32 contacts() const {
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
      if (!points[i].released) ++n;
    }
    return n;
  }

  // Drops the slots whose finger lifted last pump and clears the per-pump
  // deltas and edge flags on the ones still down. Call once per pump, before
  // feeding that pump's events.
  void BeginPump() {
    u32 kept = 0;
    for (u32 i = 0; i < count; ++i) {
      if (points[i].released) continue;
      TouchPoint& p = points[kept++];
      p = points[i];
      p.dx = 0;
      p.dy = 0;
      p.pressed = false;
    }
    for (u32 i = kept; i < count; ++i) points[i] = {};
    count = kept;
  }

  // The slot a backend event resolves to: the contact still down under this id.
  // A released slot is a tombstone for one pump and must not swallow the events
  // of a finger that came back on the same id within that pump.
  TouchPoint* LiveSlot(i64 id) {
    for (u32 i = 0; i < count; ++i) {
      if (points[i].id == id && !points[i].released) return &points[i];
    }
    return nullptr;
  }

  // Applies one contact update, positions in window pixels. A kDown past
  // kMaxPoints is dropped; a kMove/kUp for an id with no slot is ignored (the
  // finger went down before this window had focus, so there is nothing to
  // update).
  void Apply(i64 id, f32 x, f32 y, f32 pressure, Phase phase) {
    if (phase == Phase::kDown) {
      // A second down for an id already on the panel is the platform repeating
      // itself, or two panels colliding on a finger index (SDL only promises
      // the id is unique among the fingers one device currently has down).
      // Re-arm that slot: a duplicate would be a contact nothing can release,
      // since every later move and lift resolves to the first one, and it
      // survives every BeginPump because it never goes released.
      if (TouchPoint* live = LiveSlot(id)) {
        live->x = x;
        live->y = y;
        live->dx = 0;
        live->dy = 0;
        live->pressure = pressure;
        live->pressed = true;
        return;
      }
      if (count == kMaxPoints) return;
      TouchPoint& p = points[count++];
      p = {};
      p.id = id;
      p.x = x;
      p.y = y;
      p.pressure = pressure;
      p.pressed = true;
      return;
    }

    TouchPoint* p = LiveSlot(id);
    if (!p) return;

    p->dx += x - p->x;
    p->dy += y - p->y;
    p->x = x;
    p->y = y;
    p->pressure = pressure;
    if (phase == Phase::kUp) {
      p->released = true;
      p->pressure = 0;
    }
  }
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
