#!/usr/bin/env bash
# Downloads the NVIDIA DLSS (NGX) SDK into third_party/DLSS. Headers plus a
# prebuilt Linux static lib and the runtime DLSS snippet (.so). LFS smudge is
# skipped: the libs are committed as plain files, no LFS pull needed.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/third_party/DLSS"
TAG="v310.6.0"

if [ -f "$DEST/lib/Linux_x86_64/libnvsdk_ngx.a" ]; then
  echo "already present: $DEST"
  exit 0
fi

rm -rf "$DEST"
GIT_LFS_SKIP_SMUDGE=1 git clone --depth 1 --branch "$TAG" \
  https://github.com/NVIDIA/DLSS "$DEST"

echo "done: $DEST ($TAG)"
