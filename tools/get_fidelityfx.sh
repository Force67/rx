#!/usr/bin/env bash
# Downloads the AMD FidelityFX SDK (pinned tag) into third_party/FidelityFX-SDK.
# Only the FSR 3.1 upscaler sources and shaders are used; samples/media are not
# fetched (plain clone, no submodules or LFS needed).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/third_party/FidelityFX-SDK"
TAG="v1.1.4"

if [ -f "$DEST/sdk/include/FidelityFX/host/ffx_fsr3upscaler.h" ]; then
  echo "already present: $DEST"
  exit 0
fi

rm -rf "$DEST"
git clone --depth 1 --branch "$TAG" \
  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK "$DEST"

# The vk backend carves this struct out of the scratch buffer at 4-byte
# aligned offsets; the alignas(32) makes gcc emit aligned SIMD stores that
# fault on those addresses (MSVC never does). Natural alignment is correct.
sed -i 's/typedef struct alignas(32) EffectContext {/typedef struct EffectContext {/' \
  "$DEST/sdk/src/backends/vk/ffx_vk.cpp"

# The GLSL callbacks declare luma history as rgba8 but the component creates
# the resource as RGBA16F (the HLSL path is untyped, so DX never noticed).
sed -i 's/binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba8)/binding = FSR3UPSCALER_BIND_UAV_LUMA_HISTORY, rgba16f)/' \
  "$DEST/sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h"

echo "done: $DEST ($TAG)"
