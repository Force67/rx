#!/usr/bin/env bash
# Runs the d3d12 RHI acceptance tests as Windows PEs under Wine, with the
# Khronos validation layer active on the Vulkan instance underneath the
# Windows-side D3D12 runtime. On aarch64 hosts the x86_64 Wine runs through
# box64; the winevulkan -> libvulkan boundary is box64-wrapped to the native
# loader, so the host (aarch64) NVIDIA ICD and validation layer serve the
# emulated process.
#
# Two D3D12 providers run by default, selected per-process with
# WINEDLLOVERRIDES (the shared prefix is never mutated):
#   wine    Wine's builtin d3d12.dll (WineHQ vkd3d underneath). No DXR tier,
#           so compaction_test skips its ray tracing path. Strict: any Vulkan
#           validation error fails the run.
#   proton  vkd3d-proton's PE d3d12.dll/d3d12core.dll (the Proton D3D12
#           runtime, pinned release), dropped next to the test PEs and loaded
#           via d3d12,d3d12core=n. Exposes DXR 1.1, so compaction_test runs
#           the real BLAS-compaction + TLAS path. vkd3d-proton internally
#           suballocates acceleration structures and scratch out of shared
#           VkBuffer/VkDeviceMemory blocks, which trips the validation
#           layer's conservative AS-overlap checks; those two VUIDs (see
#           PROTON_WAIVED_VUIDS) are waived - rx itself places every AS and
#           scratch in its own committed resource. Any other validation
#           error fails the run.
# RX_D3D12_PROVIDER=wine|proton restricts the run to one provider.
#
# Build the PEs first (inside `nix develop`, host dxc compiles the shaders):
#   cmake -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
#     "-DRX_MODULES=core;asset;render" -DRX_RHI_VULKAN=OFF -DRX_RHI_D3D12=ON \
#     -DRX_BUILD_TESTS=ON -DRX_BUILD_RUNTIME=OFF -DRX_NRD=OFF -DRX_DLSS=OFF \
#     -DRX_FSR3=OFF -DRX_INSTALL=OFF -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
#     -DEQ_FMTLIB_DIR=<fmtlib checkout>
#   cmake --build build/mingw --target offscreen_test compaction_test fluid_sim_test
# Then, inside `nix develop` (for vkrun and Xvfb):
#   tools/wine_d3d12_test.sh [build/mingw]
#
# The wine/box64/mcfgthread tools are provisioned through nix on first run,
# pinned by this repo's flake.lock (--inputs-from) and protected from
# nix-collect-garbage by GC roots under .cache/tool-roots/; the vkd3d-proton
# PE DLLs are a pinned, checksummed release download under .cache/. Override
# with RX_WINE64 / RX_BOX64 / RX_MINGW_MCF to test a specific build.
set -euo pipefail

BUILD_DIR="${1:-build/mingw}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ROOTS="$REPO_DIR/.cache/tool-roots"
mkdir -p "$ROOTS"

# Resolves a nixpkgs attribute to a store path with a GC root, so the tool
# survives garbage collection and is re-fetched when the root dangles.
provision() {  # <root-name> <attr> [extra nix build args...]
  local name="$1" attr="$2"
  shift 2
  if [ ! -e "$ROOTS/$name" ] || [ ! -e "$(readlink -f "$ROOTS/$name")" ]; then
    nix build --inputs-from "$REPO_DIR" "nixpkgs#$attr" "$@" -o "$ROOTS/$name" \
      || nix build "nixpkgs#$attr" "$@" -o "$ROOTS/$name"
  fi
  readlink -f "$ROOTS/$name"
}

WINE64="${RX_WINE64:-$(provision wine64 wine64 --system x86_64-linux)}"
BOX64="${RX_BOX64:-$(provision box64 box64)}"
MCF="${RX_MINGW_MCF:-$(provision mcfgthread pkgsCross.mingwW64.windows.mcfgthreads)}"

# vkd3d-proton PE DLLs: the official mingw-built release (nixpkgs only carries
# the native-linking .so build). Pinned + checksummed; cached under .cache/.
VKD3D_PROTON_VERSION="2.14.1"
VKD3D_PROTON_SHA256="ac70ccfe01d610b51ca67a0a44e4f24f5da29c665facc0c2faf47e0818d23168"
VKD3D_PROTON_DIR="$REPO_DIR/.cache/vkd3d-proton-$VKD3D_PROTON_VERSION"
provision_vkd3d_proton() {
  [ -e "$VKD3D_PROTON_DIR/x64/d3d12core.dll" ] && return 0
  local url="https://github.com/HansKristian-Work/vkd3d-proton/releases/download"
  url="$url/v$VKD3D_PROTON_VERSION/vkd3d-proton-$VKD3D_PROTON_VERSION.tar.zst"
  local tar="$VKD3D_PROTON_DIR.tar.zst"
  mkdir -p "$REPO_DIR/.cache"
  curl -fsSL -o "$tar" "$url" || return 1
  echo "$VKD3D_PROTON_SHA256  $tar" | sha256sum -c --quiet - || { rm -f "$tar"; return 1; }
  rm -rf "$VKD3D_PROTON_DIR"
  mkdir -p "$VKD3D_PROTON_DIR"
  tar --use-compress-program=unzstd -xf "$tar" -C "$VKD3D_PROTON_DIR" \
    --strip-components=1 "vkd3d-proton-$VKD3D_PROTON_VERSION/x64" || { rm -rf "$VKD3D_PROTON_DIR"; return 1; }
  rm -f "$tar"
}

export WINEPREFIX="${WINEPREFIX:-$HOME/.cache/rx-wine-x64}"
# box64 needs the raw x86_64 ELF loader, not nixpkgs' bash wrapper; the
# unwrapped binary's name varies across nixpkgs wine revisions.
WINELOADER=""
for candidate in "$WINE64/bin/.wine" "$WINE64/bin/.wine64" \
                 "$WINE64/bin/.wine64-wrapped" "$WINE64/bin/wine64" "$WINE64/bin/wine"; do
  if [ -e "$candidate" ] && file -b "$candidate" | grep -q "ELF 64-bit.*x86-64"; then
    WINELOADER="$candidate"
    break
  fi
done
if [ -z "$WINELOADER" ]; then
  echo "wine_d3d12_test: no x86_64 ELF wine loader under $WINE64/bin" >&2
  exit 1
fi
export WINELOADER
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

# Prefix bootstrap: explicit wineboot on first use (instead of relying on the
# first real command to initialize it), then idempotent configuration - the
# Vulkan adapter path for wined3d and no winedbg crash dialog stealing the
# (virtual) display on a test crash.
if [ ! -e "$WINEPREFIX/system.reg" ]; then
  run_wine wineboot --init >/dev/null 2>&1
fi
run_wine reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d vulkan /f >/dev/null 2>&1
run_wine reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f \
  >/dev/null 2>&1

export RX_RHI=d3d12
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation

TESTS=(offscreen_test compaction_test fluid_sim_test)

# Validation errors vkd3d-proton's own AS suballocation causes (see header).
PROTON_WAIVED_VUIDS='VUID-vkCmdBuildAccelerationStructuresKHR-dstAccelerationStructure-03703|VUID-VkCopyAccelerationStructureInfoKHR-dst-07791'

PROVIDERS="${RX_D3D12_PROVIDER:-wine proton}"
case "$PROVIDERS" in
  wine|proton|"wine proton") ;;
  *) echo "wine_d3d12_test: RX_D3D12_PROVIDER must be wine or proton" >&2; exit 1 ;;
esac

if [[ " $PROVIDERS " == *" proton "* ]]; then
  if ! provision_vkd3d_proton; then
    if [ "$PROVIDERS" = "proton" ]; then
      echo "wine_d3d12_test: vkd3d-proton download failed" >&2
      exit 1
    fi
    echo "wine_d3d12_test: vkd3d-proton unavailable (offline?), builtin d3d12 only" >&2
    PROVIDERS="wine"
  else
    # Loaded via d3d12,d3d12core=n from the PE's own directory; the prefix's
    # system32 stays untouched.
    cp -f "$VKD3D_PROTON_DIR/x64/d3d12.dll" "$VKD3D_PROTON_DIR/x64/d3d12core.dll" \
      "$BUILD_DIR/"
  fi
fi

status=0
for provider in $PROVIDERS; do
  if [ "$provider" = proton ]; then
    overrides="d3d12,d3d12core=n"
    waived="$PROTON_WAIVED_VUIDS"
  else
    overrides="d3d12,d3d12core=b"
    waived=""
  fi
  for t in "${TESTS[@]}"; do
    if [ ! -e "$BUILD_DIR/$t.exe" ]; then
      echo "=== $t ($provider d3d12): $BUILD_DIR/$t.exe not built ==="
      status=1
      continue
    fi
    echo "=== $t ($provider d3d12, vulkan validation) ==="
    log="$(mktemp)"
    if ! WINEDLLOVERRIDES="$overrides" run_wine "$BUILD_DIR/$t.exe" >"$log" 2>&1; then
      status=1
    fi
    grep -a "$t" "$log" || true
    if [ -n "$waived" ]; then
      waived_count=$(grep -a "Validation Error" "$log" | grep -Eac "$waived" || true)
      errors=$(grep -a "Validation Error" "$log" | grep -Eavc "$waived" || true)
      [ "${waived_count:-0}" -eq 0 ] || echo "waived (vkd3d-proton AS suballocation): $waived_count"
    else
      errors=$(grep -ac "Validation Error" "$log" || true)
    fi
    echo "validation errors: $errors"
    [ "${errors:-0}" -eq 0 ] || status=1
    rm -f "$log"
  done
done
exit $status
