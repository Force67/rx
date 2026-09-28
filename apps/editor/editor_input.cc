#include "editor_input.h"

#include "rxe/ui/events/input.h"
#include "rxe/ui/events/input_bindings.h"

namespace rx {

void RegisterEditorInput(ui::InputMap& map) {
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

  map.RegisterFold(Axis::kMoveX, Action::kMoveRight, Action::kMoveLeft);
  map.RegisterFold(Axis::kMoveY, Action::kMoveBack, Action::kMoveForward);

  map.SetDefaultsFn([](ui::InputMap& m) {
    auto key = [](ui::Key k) { return ui::Binding{ui::SourceKind::kKey, static_cast<u16>(k), 0}; };
    auto pad = [](ui::GamepadButton g) {
      return ui::Binding{ui::SourceKind::kGamepadButton, static_cast<u16>(g), 0};
    };
    auto axis = [](ui::GamepadAxis a, i8 dir) {
      return ui::Binding{ui::SourceKind::kGamepadAxis, static_cast<u16>(a), dir};
    };

    m.AddBinding(Action::kMoveForward, key(ui::Key::kW));
    m.AddBinding(Action::kMoveBack, key(ui::Key::kS));
    m.AddBinding(Action::kMoveLeft, key(ui::Key::kA));
    m.AddBinding(Action::kMoveRight, key(ui::Key::kD));
    m.AddBinding(Action::kJump, key(ui::Key::kSpace));
    m.AddBinding(Action::kJump, pad(ui::GamepadButton::kSouth));
    m.AddBinding(Action::kSprint, key(ui::Key::kLeftShift));
    m.AddBinding(Action::kSprint, pad(ui::GamepadButton::kLeftStick));
    m.AddBinding(Action::kSneak, key(ui::Key::kLeftCtrl));
    m.AddBinding(Action::kSneak, pad(ui::GamepadButton::kRightStick));
    m.AddBinding(Action::kCamUp, key(ui::Key::kE));
    m.AddBinding(Action::kCamUp, pad(ui::GamepadButton::kRightShoulder));
    m.AddBinding(Action::kCamDown, key(ui::Key::kQ));
    m.AddBinding(Action::kCamDown, pad(ui::GamepadButton::kLeftShoulder));

    m.AddAxisBinding(Axis::kMoveX, axis(ui::GamepadAxis::kLeftX, 0));
    m.AddAxisBinding(Axis::kMoveY, axis(ui::GamepadAxis::kLeftY, 0));
    m.AddAxisBinding(Axis::kLookX, axis(ui::GamepadAxis::kRightX, 0));
    m.AddAxisBinding(Axis::kLookY, axis(ui::GamepadAxis::kRightY, 0));
  });
}

}  // namespace rx
