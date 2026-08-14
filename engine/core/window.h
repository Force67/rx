#ifndef RX_CORE_WINDOW_H_
#define RX_CORE_WINDOW_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/export.h"
#include "core/input.h"
#include "core/types.h"

#if defined(__ANDROID__)
struct ANativeWindow;
#endif

namespace rx {

struct WindowDesc {
  std::string title = "rx";
  u32 width = 1920;
  u32 height = 1080;
  bool fullscreen = false;
  // Render at the panel's real pixel count rather than letting the compositor
  // upscale a smaller buffer. Off is blurry on any scaled display; on means
  // width()/height() exceed the size the desktop lays the window out at, and
  // UI sized in pixels has to scale by pixel_density() to keep its physical
  // size.
  bool high_pixel_density = true;
  // When true (the platform default) a touch also emits synthetic mouse events,
  // so mouse-only UI keeps working under a finger. Handhelds want it off: with
  // mouse look in relative mode a stray thumb on the panel drags the camera.
  // Games that read touch() directly should turn it off.
  bool touch_emits_mouse = true;
};

// Opaque handles the renderer needs to create a surface. With the SDL3
// backend `window` is the SDL_Window, headless leaves both null.
struct NativeWindowHandles {
  void* window = nullptr;
  void* display = nullptr;
};

class RX_CORE_EXPORT Window {
 public:
  virtual ~Window() = default;

  virtual bool PumpEvents() = 0;
  virtual NativeWindowHandles native_handles() const = 0;
  virtual u32 width() const = 0;
  virtual u32 height() const = 0;

  // Pixels per unit of the size the desktop lays this window out at, i.e. the
  // display's scaling factor when high_pixel_density is on, otherwise 1.
  //
  // Input is already in pixels, converted by the backend, so nothing needs this
  // to hit-test. What needs it is anything whose size is authored in pixels and
  // must keep its physical size as the buffer grows: fonts, panel widths, hit
  // boxes drawn to match them. Multiply those by this.
  virtual f32 pixel_density() const { return 1.0f; }

  // Input collected by the last PumpEvents.
  const InputState& input() const { return input_; }
  const GamepadState& gamepad() const { return gamepad_; }
  const TouchState& touch() const { return touch_; }

  // Gamepad haptics. No-ops unless a pad is connected; the DualSense-only
  // effects (trigger resistance, lightbar) silently do nothing on other pads,
  // so callers can issue them unconditionally.
  virtual void SetRumble(f32 low_freq, f32 high_freq, u32 duration_ms) {}
  virtual void SetTriggerEffect(bool left, bool right, const TriggerEffect& effect) {}
  virtual void SetLedColor(u8 r, u8 g, u8 b) {}

  // While enabled the cursor is hidden and mouse_dx/dy keep accumulating
  // without hitting the screen edge. Mouse look uses this.
  virtual void SetRelativeMouseMode(bool enabled) {}
  virtual bool relative_mouse_mode() const { return false; }

  // Runtime borderless-fullscreen toggle (settings menus); headless and
  // platforms without the concept no-op and report false.
  virtual void SetFullscreen(bool enabled) { (void)enabled; }
  virtual bool fullscreen() const { return false; }

  // False while the window lacks input focus (alt-tab, another app on top);
  // games auto-pause on it. Headless stays focused.
  virtual bool focused() const { return true; }

  // True when the OS actually has HDR enabled (Windows advanced-color toggle,
  // KWin's per-output HDR setting via kde_output_device_v2, macOS EDR) - NOT
  // merely an HDR-capable display. The renderer gates its HDR swapchain
  // request on this: a Vulkan surface can advertise HDR10 formats while the
  // system toggle is off, and presenting PQ then washes out. Can flip at
  // runtime (OS setting, window moved between monitors); polled per frame.
  virtual bool hdr_enabled() const { return false; }

  // Called for every native event before the window handles it. With the
  // SDL3 backend the pointer is an SDL_Event. ImGui hooks in here.
  void set_event_hook(std::function<void(const void* native_event)> hook) {
    event_hook_ = std::move(hook);
  }

  // Vulkan glue. Backends that can present return the instance extensions
  // they need and write a VkSurfaceKHR through the opaque out pointer.
  // Headless windows return nothing, which tells the renderer to stay off.
  virtual std::vector<const char*> vulkan_instance_extensions() const { return {}; }
  virtual bool CreateVulkanSurface(void* vk_instance, void* out_vk_surface) { return false; }

  // Returns a platform window, or a headless stub when none is available.
  static std::unique_ptr<Window> Create(const WindowDesc& desc);

 protected:
  InputState input_;
  GamepadState gamepad_;
  TouchState touch_;
  std::function<void(const void*)> event_hook_;
};

#if defined(__ANDROID__)
// Android window the activity drives. The activity owns the native-glue event
// loop and feeds input and lifecycle into the window each frame, so the engine
// keeps its own Run()/RunFrame() structure unchanged.
class AndroidWindowBase : public Window {
 public:
  virtual InputState& mutable_input() = 0;
  virtual void RequestQuit() = 0;
  virtual ::ANativeWindow* native_window() const = 0;
  // Rebinds to a new ANativeWindow across the activity lifecycle (the old one
  // is released, the new one acquired). null marks the window as gone.
  virtual void SetNativeWindow(::ANativeWindow* window) = 0;
};

std::unique_ptr<AndroidWindowBase> CreateAndroidWindow(::ANativeWindow* window);
#endif

}  // namespace rx

#endif  // RX_CORE_WINDOW_H_
