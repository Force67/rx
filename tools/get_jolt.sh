#!/usr/bin/env bash
# Downloads Jolt Physics into third_party/JoltPhysics (pinned release).
# Run once, then reconfigure; cmake picks it up via RECREATION_JOLT.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/third_party/JoltPhysics"
TAG="v5.6.0"

if [ -d "$DEST/Jolt" ]; then
  CURRENT="$(git -C "$DEST" describe --tags --exact-match 2>/dev/null || true)"
  if [ "$CURRENT" = "$TAG" ] && git -C "$DEST" diff --quiet && \
     git -C "$DEST" diff --cached --quiet; then
    echo "already present: $DEST ($TAG)"
    exit 0
  fi
  echo "wrong or modified Jolt revision at $DEST: expected clean $TAG, found ${CURRENT:-unknown}" >&2
  echo "remove that checkout and rerun this script" >&2
  exit 1
fi
git clone --depth 1 --branch "$TAG" https://github.com/jrouwe/JoltPhysics.git "$DEST"
echo "done: $DEST ($TAG)"
