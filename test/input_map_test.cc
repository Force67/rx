// input_map_test: the action edges InputMap derives from raw input. An edge has
// to fire once per physical press: on the pump the source goes down, never
// again while it stays held, and also for a key tap or mouse click that went
// down and up inside a single pump, which leaves the source released by the
// end of that pump. Needs no window, so it runs in the ctest gate.

#include <stdio.h>

#include "ui/events/input.h"
#include "ui/events/input_actions.h"
#include "ui/events/input_bindings.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  ::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

enum class Action : int { kBreak, kJump, kFire };

rx::InputMap MakeMap() {
  rx::InputMap map;
  map.RegisterAction(Action::kBreak, "break");
  map.RegisterAction(Action::kJump, "jump");
  map.RegisterAction(Action::kFire, "fire");
  map.AddBinding(Action::kBreak, rx::Binding{rx::SourceKind::kMouseButton,
                                             static_cast<rx::u16>(rx::MouseButton::kLeft), 0});
  map.AddBinding(Action::kJump,
                 rx::Binding{rx::SourceKind::kKey, static_cast<rx::u16>(rx::Key::kSpace), 0});
  map.AddBinding(Action::kFire, rx::Binding{rx::SourceKind::kGamepadButton,
                                            static_cast<rx::u16>(rx::GamepadButton::kSouth), 0});
  return map;
}

rx::ActionState Pump(rx::InputMap& map, const rx::InputState& kbm,
                     const rx::GamepadState& pad = {}) {
  rx::ActionState out;
  map.Resolve(kbm, pad, rx::TouchState{}, &out);
  return out;
}

void TestHeldMouseButton() {
  ::printf("held mouse button\n");
  rx::InputMap map = MakeMap();
  const auto left = static_cast<rx::u8>(rx::MouseButton::kLeft);

  rx::InputState down;
  down.mouse[left] = true;
  down.mouse_pressed[left] = true;
  rx::ActionState a = Pump(map, down);
  Check("the pump it goes down is an edge", a.pressed(Action::kBreak) && a.down(Action::kBreak));

  rx::InputState held;
  held.mouse[left] = true;
  a = Pump(map, held);
  Check("staying down is not another edge", !a.pressed(Action::kBreak) && a.down(Action::kBreak));

  a = Pump(map, rx::InputState{});
  Check("release clears it", !a.pressed(Action::kBreak) && !a.down(Action::kBreak));
}

void TestClickInsideOnePump() {
  ::printf("click inside one pump\n");
  rx::InputMap map = MakeMap();
  const auto left = static_cast<rx::u8>(rx::MouseButton::kLeft);

  rx::InputState click;
  click.mouse_pressed[left] = true;
  click.mouse_released[left] = true;  // up again before the pump ended
  rx::ActionState a = Pump(map, click);
  Check("a click that ends released still fires once", a.pressed(Action::kBreak));
  Check("and is not reported as held", !a.down(Action::kBreak));

  a = Pump(map, rx::InputState{});
  Check("nothing on the next pump", !a.pressed(Action::kBreak));
}

void TestKeyTapInsideOnePump() {
  ::printf("key tap inside one pump\n");
  rx::InputMap map = MakeMap();
  rx::InputState tap;
  tap.pressed[static_cast<rx::u8>(rx::Key::kSpace)] = true;
  rx::ActionState a = Pump(map, tap);
  Check("a tap that ends released still fires once", a.pressed(Action::kJump));

  // Auto-repeat is not a press: it must not turn a held key into repeated edges.
  rx::InputState held;
  held.keys[static_cast<rx::u8>(rx::Key::kSpace)] = true;
  Pump(map, held);
  held.repeated[static_cast<rx::u8>(rx::Key::kSpace)] = true;
  a = Pump(map, held);
  Check("auto-repeat is not an edge", !a.pressed(Action::kJump) && a.down(Action::kJump));
}

void TestGamepadLevelEdge() {
  ::printf("gamepad level edge\n");
  rx::InputMap map = MakeMap();
  rx::GamepadState pad;
  pad.connected = true;
  pad.buttons[static_cast<rx::u8>(rx::GamepadButton::kSouth)] = true;
  rx::ActionState a = Pump(map, rx::InputState{}, pad);
  Check("gamepad press is an edge", a.pressed(Action::kFire));
  a = Pump(map, rx::InputState{}, pad);
  Check("held gamepad button is not", !a.pressed(Action::kFire) && a.down(Action::kFire));
}

enum class Axis : int { kAccel };

// Two triggers on one signed axis: right adds, left (bound with axis_dir -1)
// subtracts, and the sum is clamped like any other axis.
void TestNegatedAxisBinding() {
  ::printf("negated analog axis binding\n");
  rx::InputMap map;
  map.RegisterAxis(Axis::kAccel, "accel");
  map.AddAxisBinding(Axis::kAccel,
                     rx::Binding{rx::SourceKind::kGamepadAxis,
                                 static_cast<rx::u16>(rx::GamepadAxis::kRightTrigger), 0});
  map.AddAxisBinding(Axis::kAccel,
                     rx::Binding{rx::SourceKind::kGamepadAxis,
                                 static_cast<rx::u16>(rx::GamepadAxis::kLeftTrigger), -1});
  rx::GamepadState pad;
  pad.connected = true;
  pad.axes[static_cast<rx::u8>(rx::GamepadAxis::kRightTrigger)] = 1.0f;
  Check("right trigger drives the axis positive", Pump(map, {}, pad).axis(Axis::kAccel) > 0.99f);
  pad.axes[static_cast<rx::u8>(rx::GamepadAxis::kRightTrigger)] = 0.0f;
  pad.axes[static_cast<rx::u8>(rx::GamepadAxis::kLeftTrigger)] = 1.0f;
  Check("the negated left trigger drives it negative",
        Pump(map, {}, pad).axis(Axis::kAccel) < -0.99f);
}

}  // namespace

int main() {
  TestNegatedAxisBinding();
  TestHeldMouseButton();
  TestClickInsideOnePump();
  TestKeyTapInsideOnePump();
  TestGamepadLevelEdge();
  if (g_failures) {
    ::printf("input_map_test: %d failure(s)\n", g_failures);
    return 1;
  }
  ::printf("input_map_test: all passed\n");
  return 0;
}
