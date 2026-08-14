// input_touch_sdl_test: drives the real SDL3 finger-event path end to end.
//
// input_touch_test covers the TouchState state machine directly. This covers
// the piece it cannot: the backend translation in window_sdl3.cc, where SDL's
// normalized (0..1) finger coordinates become the window coordinates the rest
// of the input layer speaks, and where FINGER_UP/FINGER_CANCELED collapse into
// one end state. Getting that conversion wrong (swapped axes, forgotten scale,
// using the pixel size instead of the window size) is invisible to a pure logic
// test and would put every tap in the wrong place on a handheld.
//
// The pixel-size mistake is latent for now: it needs the two sizes to differ,
// and SDL keeps them equal until a window asks for a high pixel density
// backbuffer, which rx does not. The check for it skips itself while they
// agree, and starts biting the day that flag goes in.
//
// Real SDL_Event structs are pushed through SDL's own queue, so PumpEvents
// polls them exactly as it would from a panel. No touchscreen and no uinput
// device required.
//
// Needs a display to open a window; skips cleanly when there is none, so it is
// safe in a headless gate.

#include <cmath>
#include <cstdio>

#include <SDL3/SDL.h>

#include "core/window.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

bool Near(rx::f32 a, rx::f32 b, rx::f32 eps = 1.0f) {
  return std::fabs(a - b) < eps;  // a pixel of slack: normalized -> pixel rounds
}

constexpr SDL_TouchID kTouch = 1;
constexpr SDL_FingerID kFinger = 42;

// Queue a finger event the way the platform would deliver one.
void PushFinger(Uint32 type, SDL_FingerID finger, float nx, float ny, float pressure) {
  SDL_Event e{};
  e.type = type;
  e.tfinger.type = static_cast<SDL_EventType>(type);
  e.tfinger.timestamp = 0;
  e.tfinger.touchID = kTouch;
  e.tfinger.fingerID = finger;
  e.tfinger.x = nx;  // normalized to the window, as SDL reports
  e.tfinger.y = ny;
  e.tfinger.dx = 0;
  e.tfinger.dy = 0;
  e.tfinger.pressure = pressure;
  e.tfinger.windowID = 0;
  SDL_PushEvent(&e);
}

}  // namespace

int main() {
  std::printf("sdl finger event translation\n");

  rx::WindowDesc desc;
  desc.width = 640;
  desc.height = 480;
  // The point is to prove touch does NOT also move the mouse when this is off,
  // which is what the steamdeck profile relies on.
  desc.touch_emits_mouse = false;

  auto window = rx::Window::Create(desc);
  if (!window || !window->native_handles().window) {
    std::printf("  no display available, skipping\n");
    return 0;
  }

  // Window coordinates, which is what touch reports and what the mouse arrives
  // in. Window::width()/height() are the pixel size, which is the same number
  // until a window asks for a high pixel density backbuffer.
  SDL_Window* sdl = static_cast<SDL_Window*>(window->native_handles().window);
  int lw = 0, lh = 0;
  SDL_GetWindowSize(sdl, &lw, &lh);
  const rx::f32 w = static_cast<rx::f32>(lw);
  const rx::f32 h = static_cast<rx::f32>(lh);
  const rx::f32 pw = static_cast<rx::f32>(window->width());
  std::printf("  window is %.0fx%.0f, %ux%u pixels\n", w, h, window->width(), window->height());
  if (w <= 0 || h <= 0) {
    std::printf("  degenerate window, skipping\n");
    return 0;
  }

  // --- down: normalized -> pixels, on the correct axes ---
  PushFinger(SDL_EVENT_FINGER_DOWN, kFinger, 0.25f, 0.5f, 1.0f);
  window->PumpEvents();

  const rx::TouchState& t = window->touch();
  Check("a contact appeared", t.count == 1);
  if (t.count == 1) {
    Check("x scaled from normalized to window coords", Near(t.points[0].x, 0.25f * w));
    Check("y scaled from normalized to window coords", Near(t.points[0].y, 0.5f * h));
    Check("axes are not swapped", !Near(t.points[0].x, 0.25f * h) || Near(w, h));
    // Only decides anything once the two sizes differ, which needs a high
    // pixel density backbuffer. Says so rather than passing on a tautology.
    if (!Near(pw, w))
      Check("scaled by the window size, not the pixel size", !Near(t.points[0].x, 0.25f * pw));
    else
      std::printf("  [--] window and pixel size agree, the two cannot be told apart here\n");
    Check("press edge reported", t.points[0].pressed);
    Check("pressure carried through", Near(t.points[0].pressure, 1.0f, 0.01f));
    Check("id carried through", t.points[0].id == static_cast<rx::i64>(kFinger));
  }

  // Touch must not have moved the mouse: with touch_emits_mouse off SDL should
  // not be synthesizing, which is what keeps a thumb off the camera.
  Check("touch did not emit mouse motion",
        window->input().mouse_dx == 0.0f && window->input().mouse_dy == 0.0f);

  // --- motion: delta is in pixels and signed correctly ---
  PushFinger(SDL_EVENT_FINGER_MOTION, kFinger, 0.5f, 0.25f, 1.0f);
  window->PumpEvents();
  if (window->touch().count == 1) {
    const rx::TouchPoint& p = window->touch().points[0];
    Check("motion moved to the new position", Near(p.x, 0.5f * w) && Near(p.y, 0.25f * h));
    Check("motion delta is the travel in window coords",
          Near(p.dx, 0.25f * w) && Near(p.dy, -0.25f * h));
    Check("press edge cleared on the next pump", !p.pressed);
  }

  // --- cancel collapses into the same end state as a lift ---
  PushFinger(SDL_EVENT_FINGER_CANCELED, kFinger, 0.5f, 0.25f, 0.0f);
  window->PumpEvents();
  Check("canceled contact is released", window->touch().count == 1 &&
                                            window->touch().points[0].released);
  Check("released contact is not counted", window->touch().contacts() == 0);

  window->PumpEvents();
  Check("slot reclaimed on the following pump", window->touch().count == 0);

  // --- a second finger opens a second slot (multi-touch really arrives) ---
  PushFinger(SDL_EVENT_FINGER_DOWN, 1, 0.1f, 0.1f, 1.0f);
  PushFinger(SDL_EVENT_FINGER_DOWN, 2, 0.9f, 0.9f, 1.0f);
  window->PumpEvents();
  Check("two contacts tracked", window->touch().contacts() == 2);
  Check("both ids present",
        window->touch().Find(1) != nullptr && window->touch().Find(2) != nullptr);
  if (window->touch().Find(2))
    Check("second contact scaled independently", Near(window->touch().Find(2)->x, 0.9f * w));

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
