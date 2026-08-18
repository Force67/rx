#!/usr/bin/env bash
# Downloads the OpenPBR Surface example materials into assets/openpbr/.
# ~60 single-node .mtlx files (Apache-2.0) covering metals, plastics, glass,
# liquids, skin and fabric, useful for eyeballing the imported lobes against
# the values in the source documents.
# Usage: tools/get_openpbr_samples.sh [tag]   (default: the pinned tag)
# Then:  RX_MTLX=$(ls -d $PWD/assets/openpbr/open_pbr_{gold,carpaint,pearl,velvet,sand}.mtlx | paste -sd,) \
#          ./build/linux/runtime/rx --demo mtlx
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/assets/openpbr"
# The specification release rx's importer targets; see docs/OPENPBR.md.
TAG="${1:-v1.1.1}"
URL="https://codeload.github.com/AcademySoftwareFoundation/OpenPBR/tar.gz/refs/tags/$TAG"

mkdir -p "$DEST"
echo "openpbr: fetching $TAG examples into $DEST"

# One tarball rather than ~60 raw requests, and --strip-components drops both
# the "OpenPBR-<tag>" root and the "examples" directory so the .mtlx files land
# flat in $DEST.
curl -sfL "$URL" |
  tar -xz -C "$DEST" --strip-components=2 --wildcards '*/examples/*.mtlx'

count=$(find "$DEST" -maxdepth 1 -name '*.mtlx' | wc -l)
if [ "$count" -eq 0 ]; then
  echo "openpbr: no .mtlx extracted, is '$TAG' a real tag?" >&2
  exit 1
fi
echo "openpbr: $count example material(s) in $DEST"
