#include "core/wayland_kde_hdr.h"

#include <poll.h>
#include <string.h>

#include <vector>

#include <wayland-client.h>

#include "core/log.h"
#include "core/types.h"
#include "kde-output-device-v2-client-protocol.h"

namespace rx {
namespace {

struct Device {
  kde_output_device_v2* proxy = nullptr;
  bool enabled = true;  // kwin only sends `enabled` when an output is off
  bool hdr = false;
  bool removed = false;
};

// Every listener member must be non-null: libwayland calls whatever the
// compositor sends, and kwin sends most of these on bind.
void Geometry(void*, kde_output_device_v2*, i32, i32, i32, i32, i32, const char*, const char*,
              i32) {}
void CurrentMode(void*, kde_output_device_v2*, kde_output_device_mode_v2*) {}
void Mode(void*, kde_output_device_v2*, kde_output_device_mode_v2*) {}
void Done(void*, kde_output_device_v2*) {}
void Scale(void*, kde_output_device_v2*, wl_fixed_t) {}
void Edid(void*, kde_output_device_v2*, const char*) {}
void Enabled(void* data, kde_output_device_v2*, i32 enabled) {
  static_cast<Device*>(data)->enabled = enabled != 0;
}
void Uuid(void*, kde_output_device_v2*, const char*) {}
void SerialNumber(void*, kde_output_device_v2*, const char*) {}
void EisaId(void*, kde_output_device_v2*, const char*) {}
void Capabilities(void*, kde_output_device_v2*, u32) {}
void Overscan(void*, kde_output_device_v2*, u32) {}
void VrrPolicy(void*, kde_output_device_v2*, u32) {}
void RgbRange(void*, kde_output_device_v2*, u32) {}
void Name(void*, kde_output_device_v2*, const char*) {}
void HighDynamicRange(void* data, kde_output_device_v2*, u32 hdr_enabled) {
  static_cast<Device*>(data)->hdr = hdr_enabled != 0;
}
void SdrBrightness(void*, kde_output_device_v2*, u32) {}
void WideColorGamut(void*, kde_output_device_v2*, u32) {}
void AutoRotatePolicy(void*, kde_output_device_v2*, u32) {}
void IccProfilePath(void*, kde_output_device_v2*, const char*) {}
void BrightnessMetadata(void*, kde_output_device_v2*, u32, u32, u32) {}
void BrightnessOverrides(void*, kde_output_device_v2*, i32, i32, i32) {}
void SdrGamutWideness(void*, kde_output_device_v2*, u32) {}
void ColorProfileSource(void*, kde_output_device_v2*, u32) {}
void Brightness(void*, kde_output_device_v2*, u32) {}
void ColorPowerTradeoff(void*, kde_output_device_v2*, u32) {}
void Dimming(void*, kde_output_device_v2*, u32) {}
void ReplicationSource(void*, kde_output_device_v2*, const char*) {}
void DdcCiAllowed(void*, kde_output_device_v2*, u32) {}
void MaxBitsPerColor(void*, kde_output_device_v2*, u32) {}
void MaxBitsPerColorRange(void*, kde_output_device_v2*, u32, u32) {}
void AutomaticMaxBitsPerColorLimit(void*, kde_output_device_v2*, u32) {}
void EdrPolicy(void*, kde_output_device_v2*, u32) {}
void Sharpness(void*, kde_output_device_v2*, u32) {}
void Priority(void*, kde_output_device_v2*, u32) {}
void AutoBrightness(void*, kde_output_device_v2*, u32) {}
void Removed(void* data, kde_output_device_v2*) {
  static_cast<Device*>(data)->removed = true;
}
void HdrIccProfilePath(void*, kde_output_device_v2*, const char*) {}
void HdrColorProfileSource(void*, kde_output_device_v2*, u32) {}
void AbmLevel(void*, kde_output_device_v2*, u32) {}

constexpr kde_output_device_v2_listener kDeviceListener = {
    Geometry, CurrentMode, Mode, Done, Scale, Edid, Enabled, Uuid, SerialNumber, EisaId,
    Capabilities, Overscan, VrrPolicy, RgbRange, Name, HighDynamicRange, SdrBrightness,
    WideColorGamut, AutoRotatePolicy, IccProfilePath, BrightnessMetadata, BrightnessOverrides,
    SdrGamutWideness, ColorProfileSource, Brightness, ColorPowerTradeoff, Dimming,
    ReplicationSource, DdcCiAllowed, MaxBitsPerColor, MaxBitsPerColorRange,
    AutomaticMaxBitsPerColorLimit, EdrPolicy, Sharpness, Priority, AutoBrightness, Removed,
    HdrIccProfilePath, HdrColorProfileSource, AbmLevel};

}  // namespace

struct KdeOutputHdrMonitor::Impl {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  // unique_ptr so the listener user-data pointer stays stable across growth.
  std::vector<std::unique_ptr<Device>> devices;

  static void RegistryGlobal(void* data, wl_registry* registry, u32 id, const char* interface,
                             u32 version) {
    Impl* impl = static_cast<Impl*>(data);
    if (strcmp(interface, kde_output_device_v2_interface.name) != 0) return;
    auto device = std::make_unique<Device>();
    u32 bind_version = version < 23u ? version : 23u;  // what the vendored glue supports
    device->proxy = static_cast<kde_output_device_v2*>(
        wl_registry_bind(registry, id, &kde_output_device_v2_interface, bind_version));
    kde_output_device_v2_add_listener(device->proxy, &kDeviceListener, device.get());
    impl->devices.push_back(std::move(device));
  }
  static void RegistryGlobalRemove(void*, wl_registry*, u32) {}

  ~Impl() {
    for (auto& device : devices) {
      if (device->proxy) kde_output_device_v2_destroy(device->proxy);
    }
    if (registry) wl_registry_destroy(registry);
    if (display) wl_display_disconnect(display);
  }
};

std::unique_ptr<KdeOutputHdrMonitor> KdeOutputHdrMonitor::Create() {
  wl_display* display = wl_display_connect(nullptr);
  if (!display) return nullptr;

  auto monitor = std::unique_ptr<KdeOutputHdrMonitor>(new KdeOutputHdrMonitor());
  monitor->impl_ = std::make_unique<Impl>();
  Impl* impl = monitor->impl_.get();
  impl->display = display;
  impl->registry = wl_display_get_registry(display);
  static constexpr wl_registry_listener kRegistryListener = {Impl::RegistryGlobal,
                                                             Impl::RegistryGlobalRemove};
  wl_registry_add_listener(impl->registry, &kRegistryListener, impl);
  wl_display_roundtrip(display);  // globals -> device binds
  if (impl->devices.empty()) return nullptr;  // not KWin
  wl_display_roundtrip(display);  // initial per-device state
  RX_INFO("kde output monitor: {} output(s), hdr {}", impl->devices.size(),
           monitor->AnyHdrEnabled() ? "enabled" : "disabled");
  return monitor;
}

KdeOutputHdrMonitor::~KdeOutputHdrMonitor() = default;

bool KdeOutputHdrMonitor::AnyHdrEnabled() {
  Impl* impl = impl_.get();
  // Non-blocking pump: only read the socket when data is already waiting, so a
  // per-frame poll never stalls on the compositor.
  while (wl_display_prepare_read(impl->display) != 0) {
    wl_display_dispatch_pending(impl->display);
  }
  wl_display_flush(impl->display);
  pollfd pfd{wl_display_get_fd(impl->display), POLLIN, 0};
  if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
    wl_display_read_events(impl->display);
  } else {
    wl_display_cancel_read(impl->display);
  }
  wl_display_dispatch_pending(impl->display);

  std::erase_if(impl->devices, [](const std::unique_ptr<Device>& device) {
    if (!device->removed) return false;
    kde_output_device_v2_destroy(device->proxy);
    return true;
  });
  for (const auto& device : impl->devices) {
    if (device->enabled && device->hdr) return true;
  }
  return false;
}

}  // namespace rx
