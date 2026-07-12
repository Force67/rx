#!/usr/bin/env bash
# Downloads the CC0 MPFB example avatar (~35 MB glb, a MakeHuman-based head
# with 66 ARKit-style facial blend shapes) from the TalkingHead repository
# into assets/morphface/. Run once, then:
#   build/linux/runtime/rx --gltf assets/morphface/mpfb.glb
# The viewer sweeps the expression targets live (see EmitMorphedInstances).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/assets/morphface"
URL="https://raw.githubusercontent.com/met4citizen/TalkingHead/main/avatars/mpfb.glb"

mkdir -p "$DEST"
if [ -s "$DEST/mpfb.glb" ]; then
  echo "cached: $DEST/mpfb.glb"
else
  echo "fetching mpfb.glb..."
  curl -sfL "$URL" -o "$DEST/mpfb.glb"
  echo "done: $DEST/mpfb.glb"
fi
