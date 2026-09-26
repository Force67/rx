#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/algorithm.h"
#include "base/memory/move.h"
#include "base/strings/xstring.h"
#include "base/strings/format.h"
#include "core/file_system.h"
#include "core/input_bindings.h"
#include "core/scalar.h"
#include "core/text_reader.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace rx {
namespace {

// Device token tables. Each is indexed by the matching enum value, so order
// here must track the enum declarations in input.h. These are physical device
// codes, not game actions, so they stay in the engine. ---

// Bindable keys, in Key declaration order (input.h).
constexpr const char* kKeyTokens[] = {
    "W",      "A",      "S",         "D",     "Q",      "E",     "F",     "T",
    "C",      "R",      "G",         "X",     "Z",      "B",     "V",     "Space",
    "LShift", "LCtrl",  "Escape",    "F1",    "F2",     "F3",    "F4",    "F5",
    "Delete", "Backspace", "Return", "1",     "2",      "3",     "4",     "J",
    "Up",     "Down",   "Left",      "Right", "Tab",   "M",     "5",     "6",
    "L",
};
static_assert(sizeof(kKeyTokens) / sizeof(kKeyTokens[0]) == static_cast<int>(Key::kCount),
              "kKeyTokens must match the Key enum");

constexpr const char* kMouseTokens[] = {"left", "right", "middle"};
static_assert(sizeof(kMouseTokens) / sizeof(kMouseTokens[0]) ==
                  static_cast<int>(MouseButton::kCount),
              "kMouseTokens must match the MouseButton enum");

constexpr const char* kPadButtonTokens[] = {
    "south",  "east",      "west",      "north",   "back",    "guide",
    "start",  "lstick",    "rstick",    "lshoulder", "rshoulder",
    "dpup",   "dpdown",    "dpleft",    "dpright", "touchpad",
};
static_assert(sizeof(kPadButtonTokens) / sizeof(kPadButtonTokens[0]) ==
                  static_cast<int>(GamepadButton::kCount),
              "kPadButtonTokens must match the GamepadButton enum");

constexpr const char* kPadAxisTokens[] = {"leftx",    "lefty",    "rightx",
                                          "righty",   "ltrigger", "rtrigger"};
static_assert(sizeof(kPadAxisTokens) / sizeof(kPadAxisTokens[0]) ==
                  static_cast<int>(GamepadAxis::kCount),
              "kPadAxisTokens must match the GamepadAxis enum");

int FindToken(const char* name, const char* const* table, int count) {
  for (int i = 0; i < count; ++i)
    if (::strcmp(name, table[i]) == 0) return i;
  return -1;
}

}  // namespace

// Binding tokens (device-only, independent of the game's action set)

base::String BindingToken(const Binding& b) {
  switch (b.kind) {
    case SourceKind::kKey:
      if (b.code < static_cast<u16>(Key::kCount)) return base::String("key:") + kKeyTokens[b.code];
      break;
    case SourceKind::kMouseButton:
      if (b.code < static_cast<u16>(MouseButton::kCount))
        return base::String("mouse:") + kMouseTokens[b.code];
      break;
    case SourceKind::kGamepadButton:
      if (b.code < static_cast<u16>(GamepadButton::kCount))
        return base::String("pad:") + kPadButtonTokens[b.code];
      break;
    case SourceKind::kGamepadAxis:
      if (b.code < static_cast<u16>(GamepadAxis::kCount)) {
        base::String s = base::String("padaxis:") + kPadAxisTokens[b.code];
        if (b.axis_dir > 0) s += '+';
        else if (b.axis_dir < 0) s += '-';
        return s;
      }
      break;
    case SourceKind::kNone:
      break;
  }
  return "";
}

bool BindingFromToken(const char* token, Binding* out) {
  const char* colon = ::strchr(token, ':');
  if (!colon) return false;
  base::String kind(token, colon - token);
  base::String rest(colon + 1);
  Binding b;
  if (kind == "key") {
    int i = FindToken(rest.c_str(), kKeyTokens, static_cast<int>(Key::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kKey;
    b.code = static_cast<u16>(i);
  } else if (kind == "mouse") {
    int i = FindToken(rest.c_str(), kMouseTokens, static_cast<int>(MouseButton::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kMouseButton;
    b.code = static_cast<u16>(i);
  } else if (kind == "pad") {
    int i = FindToken(rest.c_str(), kPadButtonTokens, static_cast<int>(GamepadButton::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kGamepadButton;
    b.code = static_cast<u16>(i);
  } else if (kind == "padaxis") {
    i8 dir = 0;
    if (!rest.empty() && (rest.back() == '+' || rest.back() == '-')) {
      dir = rest.back() == '+' ? 1 : -1;
      rest.pop_back();
    }
    int i = FindToken(rest.c_str(), kPadAxisTokens, static_cast<int>(GamepadAxis::kCount));
    if (i < 0) return false;
    b.kind = SourceKind::kGamepadAxis;
    b.code = static_cast<u16>(i);
    b.axis_dir = dir;
  } else {
    return false;
  }
  *out = b;
  return true;
}

base::String BindingLabel(const Binding& b) {
  switch (b.kind) {
    case SourceKind::kKey:
      return b.code < static_cast<u16>(Key::kCount) ? kKeyTokens[b.code] : "?";
    case SourceKind::kMouseButton:
      return b.code < static_cast<u16>(MouseButton::kCount)
                 ? base::String("Mouse ") + kMouseTokens[b.code]
                 : "?";
    case SourceKind::kGamepadButton:
      return b.code < static_cast<u16>(GamepadButton::kCount)
                 ? base::String("Pad ") + kPadButtonTokens[b.code]
                 : "?";
    case SourceKind::kGamepadAxis: {
      if (b.code >= static_cast<u16>(GamepadAxis::kCount)) return "?";
      base::String s = base::String("Pad ") + kPadAxisTokens[b.code];
      if (b.axis_dir > 0) s += "+";
      else if (b.axis_dir < 0) s += "-";
      return s;
    }
    case SourceKind::kNone:
      break;
  }
  return "(unbound)";
}

// Schema registration

void InputMap::RegisterActionId(ActionId id, const char* ini_name) {
  if (id >= kMaxActions) return;
  action_names_[id] = ini_name;
  if (id + 1 > action_count_) action_count_ = id + 1;
}

void InputMap::RegisterAxisId(AxisId id, const char* ini_name) {
  if (id >= kMaxAxes) return;
  axis_names_[id] = ini_name;
  if (id + 1 > axis_count_) axis_count_ = id + 1;
}

void InputMap::RegisterFoldId(AxisId axis, ActionId positive, ActionId negative) {
  folds_.push_back({axis, positive, negative});
}

void InputMap::SetDefaultsFn(base::Function<void(InputMap&)> fn) {
  defaults_ = base::move(fn);
  LoadDefaults();
}

const char* InputMap::ActionName(ActionId a) const {
  return a < kMaxActions ? action_names_[a] : nullptr;
}

const char* InputMap::AxisName(AxisId a) const {
  return a < kMaxAxes ? axis_names_[a] : nullptr;
}

bool InputMap::ActionFromName(const char* name, ActionId* out) const {
  for (int i = 0; i < action_count_; ++i)
    if (action_names_[i] && ::strcmp(name, action_names_[i]) == 0) {
      *out = static_cast<ActionId>(i);
      return true;
    }
  return false;
}

bool InputMap::AxisFromName(const char* name, AxisId* out) const {
  for (int i = 0; i < axis_count_; ++i)
    if (axis_names_[i] && ::strcmp(name, axis_names_[i]) == 0) {
      *out = static_cast<AxisId>(i);
      return true;
    }
  return false;
}

void InputMap::LoadDefaults() {
  for (auto& v : action_) v.clear();
  for (auto& v : axis_) v.clear();
  if (defaults_) defaults_(*this);
}

void InputMap::AddActionBinding(ActionId a, Binding b) {
  if (a >= kMaxActions) return;
  auto& v = action_[a];
  if (static_cast<int>(v.size()) >= kMaxBindings) return;
  if (base::Find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
}

void InputMap::AddAxisBindingId(AxisId a, Binding b) {
  if (a >= kMaxAxes) return;
  auto& v = axis_[a];
  if (static_cast<int>(v.size()) >= kMaxBindings) return;
  if (base::Find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
}

namespace {
// Keyboard and mouse share a "family"; gamepad button/axis share the other. A
// rebind replaces only the same-family binding so the cross-device pairing
// (e.g. keyboard + pad) survives.
int SourceFamily(SourceKind k) {
  return (k == SourceKind::kKey || k == SourceKind::kMouseButton) ? 0 : 1;
}
}  // namespace

void InputMap::RebindAction(ActionId a, Binding b) {
  if (a >= kMaxActions) return;
  // Remove this source from every action so it isn't bound twice.
  for (auto& v : action_)
    base::EraseIf(v, [&](const Binding& x) { return x == b; });
  // Drop the target's existing same-family binding, then add the new one.
  auto& t = action_[a];
  const int fam = SourceFamily(b.kind);
  base::EraseIf(t, [&](const Binding& x) { return SourceFamily(x.kind) == fam; });
  if (static_cast<int>(t.size()) < kMaxBindings) t.push_back(b);
  else t.back() = b;
}

void InputMap::ResetToDefaults() {
  LoadDefaults();
  look_sens_kbm = 0.0025f;
  look_sens_pad = 2.6f;
  invert_y = false;
  stick_deadzone = 0.15f;
  trigger_threshold = 0.5f;
  rumble = true;
  adaptive_triggers = true;
  led_r = 0;
  led_g = 60;
  led_b = 120;
}

ActionId InputMap::ConflictingAction(const Binding& b) const {
  for (int a = 0; a < action_count_; ++a)
    if (base::Find(action_[a].begin(), action_[a].end(), b) != action_[a].end())
      return static_cast<ActionId>(a);
  return static_cast<ActionId>(action_count_);
}

// Resolution

bool InputMap::SourceHeld(const Binding& b, const InputState& kbm, const GamepadState& pad) const {
  switch (b.kind) {
    case SourceKind::kKey:
      return b.code < static_cast<u16>(Key::kCount) && kbm.keys[b.code];
    case SourceKind::kMouseButton:
      return b.code < static_cast<u16>(MouseButton::kCount) && kbm.mouse[b.code];
    case SourceKind::kGamepadButton:
      return pad.connected && b.code < static_cast<u16>(GamepadButton::kCount) &&
             pad.buttons[b.code];
    case SourceKind::kGamepadAxis: {
      if (!pad.connected || b.code >= static_cast<u16>(GamepadAxis::kCount)) return false;
      f32 v = pad.axes[b.code];
      bool trigger = b.code == static_cast<u16>(GamepadAxis::kLeftTrigger) ||
                     b.code == static_cast<u16>(GamepadAxis::kRightTrigger);
      f32 thr = trigger ? trigger_threshold : stick_deadzone;
      if (b.axis_dir > 0) return v > thr;
      if (b.axis_dir < 0) return v < -thr;
      return ::fabs(v) > thr;
    }
    case SourceKind::kNone:
      return false;
  }
  return false;
}

f32 InputMap::AxisValue(const Binding& b, const GamepadState& pad) const {
  if (b.kind != SourceKind::kGamepadAxis || !pad.connected ||
      b.code >= static_cast<u16>(GamepadAxis::kCount))
    return 0;
  f32 v = pad.axes[b.code];
  if (::fabs(v) < stick_deadzone) return 0;
  // Rescale so the value ramps from 0 at the deadzone edge to 1 at full throw.
  f32 s = (::fabs(v) - stick_deadzone) / (1.0f - stick_deadzone);
  s = rx::Clamp(s, 0.0f, 1.0f);
  return ::copysign(s, v);
}

void InputMap::Resolve(const InputState& kbm, const GamepadState& pad, const TouchState& touch,
                       ActionState* out) {
  // Analog axes from their gamepad sources (summed, then clamped).
  for (int a = 0; a < axis_count_; ++a) {
    f32 v = 0;
    for (const Binding& b : axis_[a]) v += AxisValue(b, pad);
    out->analog[a] = rx::Clamp(v, -1.0f, 1.0f);
  }

  // Digital actions: a source held => action held; edge from the previous pump.
  for (int a = 0; a < action_count_; ++a) {
    bool held = false;
    for (const Binding& b : action_[a])
      if (SourceHeld(b, kbm, pad)) {
        held = true;
        break;
      }
    out->held[a] = held;
    out->edge[a] = held && !prev_held_[a];
    prev_held_[a] = held;
  }

  // Fold each registered digital +/- action pair into its analog axis, so
  // gameplay reads keyboard movement and stick deflection as one value.
  for (const Fold& f : folds_) {
    f32 v = out->analog[f.axis];
    if (out->held[f.positive]) v += 1.0f;
    if (out->held[f.negative]) v -= 1.0f;
    out->analog[f.axis] = rx::Clamp(v, -1.0f, 1.0f);
  }

  // Track the most recently active device for prompt glyphs.
  bool kbm_active = kbm.text_len > 0 || kbm.mouse_dx != 0 || kbm.mouse_dy != 0 || kbm.wheel != 0;
  for (int k = 0; !kbm_active && k < static_cast<int>(Key::kCount); ++k)
    if (kbm.pressed[k]) kbm_active = true;
  for (int m = 0; !kbm_active && m < static_cast<int>(MouseButton::kCount); ++m)
    if (kbm.mouse[m]) kbm_active = true;
  bool pad_active = false;
  if (pad.connected) {
    for (int b = 0; !pad_active && b < static_cast<int>(GamepadButton::kCount); ++b)
      if (pad.buttons[b]) pad_active = true;
    for (int x = 0; !pad_active && x < static_cast<int>(GamepadAxis::kCount); ++x) {
      bool trigger = x == static_cast<int>(GamepadAxis::kLeftTrigger) ||
                     x == static_cast<int>(GamepadAxis::kRightTrigger);
      f32 thr = trigger ? trigger_threshold : stick_deadzone;
      if (::fabs(pad.axes[x]) > thr) pad_active = true;
    }
  }
  // A finger landing wins over a stale pointer position: on a handheld the
  // touchscreen and the pad are both live, so only a fresh press should flip
  // the prompt glyphs.
  bool touch_active = false;
  for (u32 t = 0; !touch_active && t < touch.count; ++t)
    if (touch.points[t].pressed) touch_active = true;

  if (touch_active) last_device_ = InputDevice::kTouch;
  else if (pad_active) last_device_ = InputDevice::kGamepad;
  else if (kbm_active) last_device_ = InputDevice::kKeyboardMouse;
  out->last_device = last_device_;
}

// Persistence

namespace {

base::String Trim(const base::String& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == base::String::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

base::String InputMap::DefaultConfigPath() {
#if defined(_WIN32)
  if (const char* appdata = ::getenv("APPDATA"))
    return base::String(appdata) + "\\rx\\controls.ini";
  return "";
#elif defined(__APPLE__)
  if (const char* home = ::getenv("HOME"))
    return base::String(home) + "/Library/Application Support/rx/controls.ini";
  return "";
#else
  if (const char* xdg = ::getenv("XDG_CONFIG_HOME"))
    return base::String(xdg) + "/rx/controls.ini";
  if (const char* home = ::getenv("HOME"))
    return base::String(home) + "/.config/rx/controls.ini";
  return "";
#endif
}

bool InputMap::LoadFromIni(const base::String& path) {
  base::String text;
  if (!fs::ReadTextFile(path, &text)) return false;

  base::String section;
  LineReader lines(text);
  base::StringRef line;
  while (lines.Next(&line)) {
    base::String t = Trim(base::String(line.data(), line.size()));
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    if (t[0] == '[' && t.back() == ']') {
      section = t.substr(1, t.size() - 2);
      continue;
    }
    size_t eq = t.find('=');
    if (eq == base::String::npos) continue;
    base::String key = Trim(t.substr(0, eq));
    base::String value = Trim(t.substr(eq + 1));

    if (section == "options") {
      if (key == "look_sens_kbm") look_sens_kbm = ::strtof(value.c_str(), nullptr);
      else if (key == "look_sens_pad") look_sens_pad = ::strtof(value.c_str(), nullptr);
      else if (key == "invert_y") invert_y = value == "1" || value == "true";
      else if (key == "stick_deadzone") stick_deadzone = ::strtof(value.c_str(), nullptr);
      else if (key == "trigger_threshold") trigger_threshold = ::strtof(value.c_str(), nullptr);
      else if (key == "rumble") rumble = value == "1" || value == "true";
      else if (key == "adaptive_triggers") adaptive_triggers = value == "1" || value == "true";
      else if (key == "led" && value.size() == 6) {
        auto hex = [&](int i) { return static_cast<u8>(::strtol(value.substr(i, 2).c_str(), nullptr, 16)); };
        led_r = hex(0);
        led_g = hex(2);
        led_b = hex(4);
      }
    } else if (section == "actions") {
      ActionId a;
      if (!ActionFromName(key.c_str(), &a)) continue;
      action_[a].clear();  // file overrides the default for this action
      LineReader tokens(value, ',');
      base::StringRef piece;
      while (tokens.Next(&piece)) {
        base::String tok = Trim(base::String(piece.data(), piece.size()));
        Binding b;
        if (!tok.empty() && BindingFromToken(tok.c_str(), &b)) AddActionBinding(a, b);
      }
    } else if (section == "axes") {
      AxisId ax;
      if (!AxisFromName(key.c_str(), &ax)) continue;
      axis_[ax].clear();
      LineReader tokens(value, ',');
      base::StringRef piece;
      while (tokens.Next(&piece)) {
        base::String tok = Trim(base::String(piece.data(), piece.size()));
        Binding b;
        if (!tok.empty() && BindingFromToken(tok.c_str(), &b)) AddAxisBindingId(ax, b);
      }
    }
  }
  return true;
}

bool InputMap::SaveToIni(const base::String& path) const {
  base::String out;
  out.append("# rx controls. Tokens: key:W mouse:left pad:south padaxis:lefty+\n");
  out.append("[options]\n");
  // {} on a float is printf's %g, which is what the ostream this replaced
  // wrote, so existing files round-trip byte for byte.
  base::FormatTo(out, "look_sens_kbm={}\n", look_sens_kbm);
  base::FormatTo(out, "look_sens_pad={}\n", look_sens_pad);
  base::FormatTo(out, "invert_y={}\n", invert_y ? 1 : 0);
  base::FormatTo(out, "stick_deadzone={}\n", stick_deadzone);
  base::FormatTo(out, "trigger_threshold={}\n", trigger_threshold);
  base::FormatTo(out, "rumble={}\n", rumble ? 1 : 0);
  base::FormatTo(out, "adaptive_triggers={}\n", adaptive_triggers ? 1 : 0);
  char led[8];
  ::snprintf(led, sizeof(led), "%02x%02x%02x", led_r, led_g, led_b);
  base::FormatTo(out, "led={}\n\n", led);

  auto write_tokens = [&out](const base::Vector<Binding>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) out.append(",");
      out.append(BindingToken(v[i]));
    }
  };

  out.append("[actions]\n");
  for (int a = 0; a < action_count_; ++a) {
    if (!action_names_[a]) continue;
    base::FormatTo(out, "{}=", action_names_[a]);
    write_tokens(action_[a]);
    out.append("\n");
  }
  out.append("\n[axes]\n");
  for (int x = 0; x < axis_count_; ++x) {
    if (!axis_names_[x]) continue;
    base::FormatTo(out, "{}=", axis_names_[x]);
    write_tokens(axis_[x]);
    out.append("\n");
  }
  return fs::WriteTextFile(path, out);
}

}  // namespace rx
