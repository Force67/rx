#ifndef RX_CORE_INPUT_BINDINGS_H_
#define RX_CORE_INPUT_BINDINGS_H_

#include <functional>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/input_actions.h"
#include "core/types.h"

namespace rx {

// A single physical source that drives an action or axis. For kGamepadAxis,
// axis_dir picks a half-axis when the binding feeds a digital Action (+1 = the
// positive half, e.g. stick-down/right past the deadzone), or 0 when it feeds an
// analog Axis with its full signed value.
enum class SourceKind : u8 { kNone, kKey, kMouseButton, kGamepadButton, kGamepadAxis };

struct Binding {
  SourceKind kind = SourceKind::kNone;
  u16 code = 0;     // Key / MouseButton / GamepadButton / GamepadAxis as integer
  i8 axis_dir = 0;  // gamepad-axis half-axis sign for digital use; 0 for analog

  bool operator==(const Binding& o) const {
    return kind == o.kind && code == o.code && axis_dir == o.axis_dir;
  }
};

// Owns the action/axis binding tables and the look/deadzone options, resolves
// raw input into an ActionState each pump, and serialises to a controls INI.
// One instance lives for the session; Resolve keeps the previous held state so
// it can report per-action press edges regardless of the source device.
//
// The map knows *how* to resolve and persist input but not *which* actions
// exist: the application registers its action/axis set (stable INI name per id,
// the digital->analog folds, and a default-binding routine) at startup, then
// drives resolution and rebinding by its own enum values. This keeps every
// game-specific verb, screen, and key default out of the engine.
class InputMap {
 public:
  static constexpr int kMaxBindings = 3;  // per action/axis (primary + alternates)

  // Empty until the application registers its schema and defaults.
  InputMap() = default;

  // --- Schema registration (call once at startup, before Resolve / LoadFromIni)
  // Registers an action/axis id with the stable token used in controls.ini and
  // the rebind UI. `id` is the game's enum value; names must round-trip old INIs.
  template <class A>
  void RegisterAction(A id, const char* ini_name) {
    RegisterActionId(static_cast<ActionId>(id), ini_name);
  }
  template <class X>
  void RegisterAxis(X id, const char* ini_name) {
    RegisterAxisId(static_cast<AxisId>(id), ini_name);
  }
  // Folds a digital +/- action pair into an analog axis, so keyboard movement
  // reads as one continuous value alongside the stick (e.g. move_x from
  // move_right / move_left).
  template <class X, class A>
  void RegisterFold(X axis, A positive, A negative) {
    RegisterFoldId(static_cast<AxisId>(axis), static_cast<ActionId>(positive),
                   static_cast<ActionId>(negative));
  }
  // The application's default-binding routine (it calls AddBinding/AddAxisBinding
  // for its whole set). Stored so ResetToDefaults() can reapply it, and invoked
  // immediately to seed the built-in layout.
  void SetDefaultsFn(std::function<void(InputMap&)> fn);

  // Reapplies the registered defaults (bindings only).
  void LoadDefaults();

  // Builds `out` from this pump's raw input. Non-const: tracks press edges and
  // the last active device across calls.
  void Resolve(const InputState& kbm, const GamepadState& pad, ActionState* out);

  // --- Rebinding (used by the settings UI) ---
  template <class A>
  const std::vector<Binding>& bindings(A a) const {
    return action_[static_cast<int>(a)];
  }
  template <class X>
  const std::vector<Binding>& axis_bindings(X a) const {
    return axis_[static_cast<int>(a)];
  }
  template <class A>
  void ClearBindings(A a) {
    action_[static_cast<int>(a)].clear();
  }
  template <class X>
  void ClearAxisBindings(X a) {
    axis_[static_cast<int>(a)].clear();
  }
  // Adds a binding (capped at kMaxBindings, drops duplicates).
  template <class A>
  void AddBinding(A a, Binding b) {
    AddActionBinding(static_cast<ActionId>(a), b);
  }
  template <class X>
  void AddAxisBinding(X a, Binding b) {
    AddAxisBindingId(static_cast<AxisId>(a), b);
  }
  // Rebinds an action to `b`: removes `b` from every other action (no dup),
  // replaces `a`'s existing same-family binding (keyboard/mouse vs gamepad), and
  // keeps the other-device binding. Used by the settings capture flow.
  template <class A>
  void Rebind(A a, Binding b) {
    RebindAction(static_cast<ActionId>(a), b);
  }
  // Restores the built-in bindings and the look/deadzone options.
  void ResetToDefaults();
  // Returns the action id this source is already bound to, or the registered
  // action count if free. Lets the UI warn about / clear conflicts before a
  // rebind.
  ActionId ConflictingAction(const Binding& b) const;

  int action_count() const { return action_count_; }
  int axis_count() const { return axis_count_; }

  // --- Options (persisted) ---
  f32 look_sens_kbm = 0.0025f;  // radians per mouse pixel
  f32 look_sens_pad = 2.6f;     // radians per second at full stick deflection
  bool invert_y = false;
  f32 stick_deadzone = 0.15f;
  f32 trigger_threshold = 0.5f;  // pull past this to fire a digital action
  bool rumble = true;
  bool adaptive_triggers = true;
  u8 led_r = 0, led_g = 60, led_b = 120;  // DualSense lightbar

  // --- Persistence ---
  bool LoadFromIni(const std::string& path);
  bool SaveToIni(const std::string& path) const;

  // Default controls path: $XDG_CONFIG_HOME/rx/controls.ini (or the
  // platform equivalent). Empty if no home directory can be found.
  static std::string DefaultConfigPath();

 private:
  struct Fold {
    AxisId axis;
    ActionId positive;
    ActionId negative;
  };

  void RegisterActionId(ActionId id, const char* ini_name);
  void RegisterAxisId(AxisId id, const char* ini_name);
  void RegisterFoldId(AxisId axis, ActionId positive, ActionId negative);

  void AddActionBinding(ActionId a, Binding b);
  void AddAxisBindingId(AxisId a, Binding b);
  void RebindAction(ActionId a, Binding b);

  // Registered INI name for an id, or nullptr / false when unregistered.
  const char* ActionName(ActionId a) const;
  const char* AxisName(AxisId a) const;
  bool ActionFromName(const char* name, ActionId* out) const;
  bool AxisFromName(const char* name, AxisId* out) const;

  bool SourceHeld(const Binding& b, const InputState& kbm, const GamepadState& pad) const;
  f32 AxisValue(const Binding& b, const GamepadState& pad) const;

  std::vector<Binding> action_[kMaxActions];
  std::vector<Binding> axis_[kMaxAxes];
  bool prev_held_[kMaxActions] = {};

  const char* action_names_[kMaxActions] = {};
  const char* axis_names_[kMaxAxes] = {};
  int action_count_ = 0;  // one past the highest registered action id
  int axis_count_ = 0;    // one past the highest registered axis id
  std::vector<Fold> folds_;
  std::function<void(InputMap&)> defaults_;

  InputDevice last_device_ = InputDevice::kKeyboardMouse;
};

// Source token, e.g. "key:W", "mouse:left", "pad:south", "padaxis:lefty+".
std::string BindingToken(const Binding& b);
bool BindingFromToken(const char* token, Binding* out);
// Short label for a binding as shown next to an action in the settings list.
std::string BindingLabel(const Binding& b);

}  // namespace rx

#endif  // RX_CORE_INPUT_BINDINGS_H_
