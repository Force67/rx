#!/usr/bin/env bash
# Runs the d3d12 RHI acceptance tests as Windows PEs under Wine, with the
# Khronos validation layer active on the Vulkan instance Wine's d3d12.dll
# creates underneath. On aarch64 hosts the x86_64 Wine runs through box64;
# the winevulkan -> libvulkan boundary is box64-wrapped to the native loader,
# so the host (aarch64) NVIDIA ICD and validation layer serve the emulated
# process.
#
# Build the PEs first (inside `nix develop`, host dxc compiles the shaders):
#   cmake -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#     "-DRX_MODULES=core;asset;render" -DRX_RHI_VULKAN=OFF -DRX_RHI_D3D12=ON \
#     -DRX_BUILD_TESTS=ON -DRX_BUILD_RUNTIME=OFF -DRX_NRD=OFF -DRX_DLSS=OFF \
#     -DRX_FSR3=OFF -DRX_INSTALL=OFF -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
#     -DEQ_FMTLIB_DIR=<fmtlib checkout>
#   cmake --build build/mingw --target offscreen_test compaction_test
# Then, inside `nix develop` (for vkrun and Xvfb):
#   tools/wine_d3d12_test.sh [build/mingw]
#
# Store paths are pinned like cmake/toolchain-mingw-w64.cmake; override with
# RX_WINE64 / RX_BOX64 / RX_MINGW_MCF when they drift.
set -euo pipefail

BUILD_DIR="${1:-build/mingw}"
WINE64="${RX_WINE64:-/nix/store/4813hlbs1q9smib1109sxvm9251yqbjb-wine64-11.0}"
BOX64="${RX_BOX64:-/nix/store/rh5nr388whprv8r61bimvpc5frkwddx5-box64-0.4.2}"
MCF="${RX_MINGW_MCF:-/nix/store/y8iv45r55ryaxgwdxjkbbprmcd8jyl91-mcfgthread-x86_64-w64-mingw32-2.4.1}"

export WINEPREFIX="${WINEPREFIX:-$HOME/.cache/rx-wine-x64}"
export WINELOADER="$WINE64/bin/.wine"
export WINEDEBUG="${WINEDEBUG:--all}"
# The mingw test binaries link the mcfgthread runtime DLL.
export WINEPATH="Z:$(sed 's|/|\\\\|g' <<<"$MCF")\\\\bin"

run_wine() {
  if [ "$(uname -m)" = "aarch64" ]; then
    vkrun timeout 300 "$BOX64/bin/box64" "$WINELOADER" "$@"
  else
    vkrun timeout 300 "$WINELOADER" "$@"
  fi
}

# Wine needs a display for winevulkan/dxgi; drive a private Xvfb when
# headless. wined3d must enumerate adapters through Vulkan (the GL path has
# no pixel formats under Xvfb).
if [ -z "${DISPLAY:-}" ]; then
  Xvfb :97 -screen 0 1280x720x24 >/dev/null 2>&1 &
  XVFB_PID=$!
  trap 'kill $XVFB_PID 2>/dev/null || true' EXIT
  sleep 2
  export DISPLAY=:97
fi
run_wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d vulkan /f >/dev/null 2>&1

export RX_RHI=d3d12
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation

status=0
for t in offscreen_test compaction_test; do
  echo "=== $t (wine d3d12, vulkan validation) ==="
  log="$(mktemp)"
  if ! run_wine "$BUILD_DIR/$t.exe" >"$log" 2>&1; then
    status=1
  fi
  grep -a "$t" "$log" || true
  errors=$(grep -ac "Validation Error" "$log" || true)
  echo "validation errors: $errors"
  [ "${errors:-0}" -eq 0 ] || status=1
  rm -f "$log"
done
exit $status
