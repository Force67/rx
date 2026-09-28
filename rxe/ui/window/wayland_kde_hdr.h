#ifndef RX_UI_WINDOW_WAYLAND_KDE_HDR_H_
#define RX_UI_WINDOW_WAYLAND_KDE_HDR_H_

#include "base/memory/unique_pointer.h"


namespace rx::ui {

// Reads the per-output HDR toggle KWin exposes over the kde_output_device_v2
// wayland protocol (the same source `kscreen-doctor -o` prints). This is the
// only client-visible mirror of the KDE display setting: the standard
// color-management protocol does not carry an HDR bit, and SDL's HDR window
// property derives from luminance headroom, which KWin reports > 1 even for
// SDR outputs (a dimmed SDR panel shows headroom ~3x). Uses its own wayland
// connection so it never interferes with SDL's event dispatch.
class KdeOutputHdrMonitor {
 public:
  // Connects and takes the initial output snapshot. Returns null when there is
  // no wayland display or the compositor is not KWin (no kde_output_device_v2
  // globals) - callers then fall back to SDL's property.
  static base::UniquePointer<KdeOutputHdrMonitor> Create();
  ~KdeOutputHdrMonitor();

  KdeOutputHdrMonitor(const KdeOutputHdrMonitor&) = delete;
  KdeOutputHdrMonitor& operator=(const KdeOutputHdrMonitor&) = delete;

  // Pumps pending protocol events (non-blocking) and returns whether any
  // enabled output has HDR turned on in the system settings. Deliberately not
  // matched to the window's output: HDR anywhere means the user wants HDR, and
  // fullscreen games sit on that display in practice.
  bool AnyHdrEnabled();

 private:
  KdeOutputHdrMonitor() = default;

  struct Impl;
  base::UniquePointer<Impl> impl_;
};

}  // namespace rx::ui

#endif  // RX_UI_WINDOW_WAYLAND_KDE_HDR_H_
