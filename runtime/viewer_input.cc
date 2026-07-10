#include "viewer_input.h"

#include "core/input.h"
#include "core/input_bindings.h"

// The viewer's input policy: the action/axis names that round-trip to
// controls.ini, the digital->analog movement folds, and the built-in
// keyboard/mouse + gamepad bindings. All of this is a game decision, so it lives
// here rather than in the engine.
namespace rx {

void RegisterViewerInput(InputMap& map) {
  map.RegisterAction(Action::kMoveForward, "move_forward");
  map.RegisterAction(Action::kMoveBack, "move_back");
  map.RegisterAction(Action::kMoveLeft, "move_left");
  map.RegisterAction(Action::kMoveRight, "move_right");
  map.RegisterAction(Action::kJump, "jump");
  map.RegisterAction(Action::kSprint, "sprint");
  map.RegisterAction(Action::kSneak, "sneak");
  map.RegisterAction(Action::kCamUp, "cam_up");
  map.RegisterAction(Action::kCamDown, "cam_down");
  map.RegisterAction(Action::kToggleDebug, "toggle_debug");
  map.RegisterAction(Action::kThrowDebug, "throw_debug");

  map.RegisterAxis(Axis::kMoveX, "move_x");
  map.RegisterAxis(Axis::kMoveY, "move_y");
  map.RegisterAxis(Axis::kLookX, "look_x");
  map.RegisterAxis(Axis::kLookY, "look_y");

  // Stick convention is SDL's (down/right positive), so forward subtracts on Y.
  map.RegisterFold(Axis::kMoveX, Action::kMoveRight, Action::kMoveLeft);
  map.RegisterFold(Axis::kMoveY, Action::kMoveBack, Action::kMoveForward);

  map.SetDefaultsFn([](InputMap& m) {
    auto key = [](Key k) { return Binding{SourceKind::kKey, static_cast<u16>(k), 0}; };
    auto pad = [](GamepadButton g) {
      return Binding{SourceKind::kGamepadButton, static_cast<u16>(g), 0};
    };
    auto axis = [](GamepadAxis a, i8 dir) {
      return Binding{SourceKind::kGamepadAxis, static_cast<u16>(a), dir};
    };

    m.AddBinding(Action::kMoveForward, key(Key::kW));
    m.AddBinding(Action::kMoveBack, key(Key::kS));
    m.AddBinding(Action::kMoveLeft, key(Key::kA));
    m.AddBinding(Action::kMoveRight, key(Key::kD));
    m.AddBinding(Action::kJump, key(Key::kSpace));
    m.AddBinding(Action::kJump, pad(GamepadButton::kSouth));
    m.AddBinding(Action::kSprint, key(Key::kLeftShift));
    m.AddBinding(Action::kSprint, pad(GamepadButton::kLeftStick));
    m.AddBinding(Action::kSneak, key(Key::kLeftCtrl));
    m.AddBinding(Action::kSneak, pad(GamepadButton::kRightStick));
    m.AddBinding(Action::kCamUp, key(Key::kE));
    m.AddBinding(Action::kCamUp, pad(GamepadButton::kRightShoulder));
    m.AddBinding(Action::kCamDown, key(Key::kQ));
    m.AddBinding(Action::kCamDown, pad(GamepadButton::kLeftShoulder));
    m.AddBinding(Action::kToggleDebug, key(Key::kF1));
    m.AddBinding(Action::kThrowDebug, key(Key::kF));

    m.AddAxisBinding(Axis::kMoveX, axis(GamepadAxis::kLeftX, 0));
    m.AddAxisBinding(Axis::kMoveY, axis(GamepadAxis::kLeftY, 0));
    m.AddAxisBinding(Axis::kLookX, axis(GamepadAxis::kRightX, 0));
    m.AddAxisBinding(Axis::kLookY, axis(GamepadAxis::kRightY, 0));
  });
}

}  // namespace rx
