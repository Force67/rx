# plasma-wayland protocol glue

Pregenerated wayland-scanner client code for KDE's `kde-output-device-v2`
protocol (MIT-CMU, upstream: plasma-wayland-protocols). Used by
`engine/core/wayland_kde_hdr.cc` to read the per-output HDR toggle KWin
exposes (the same source `kscreen-doctor -o` prints), because the standard
color-management protocol does not carry the toggle and SDL's HDR properties
derive from luminance headroom, which KWin reports > 1 even in SDR mode.

Regenerate with:
  wayland-scanner client-header kde-output-device-v2.xml kde-output-device-v2-client-protocol.h
  wayland-scanner private-code  kde-output-device-v2.xml kde-output-device-v2-protocol.c
