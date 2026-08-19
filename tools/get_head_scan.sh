#!/usr/bin/env bash
# Fetches a subject for the character reference lab (`--demo lookdev`).
#
#   tools/get_head_scan.sh [scan|head|face]      (default: scan)
#
#   scan  RenderPeople's free sample "Dennis" (~133 MB download): a
#         photogrammetry-scanned human with 8K diffuse and 8K tangent-space
#         normal maps and ~100k triangles. This is the high-fidelity subject -
#         the pore-level normal map is what actually exercises the dual
#         specular lobe, the separate diffuse/specular normals and the
#         roughness-aware mip behaviour. Free sample, RenderPeople's own terms.
#         Ships as OBJ, so tools/obj_to_glb.py converts it (metres, facing -Z).
#
#   head  The Lee Perry-Smith head (Infinite-Realities, CC-BY 3.0), as mirrored
#         by three.js: a scan-derived head with 1K colour/normal/specular maps.
#         Small, loads instantly, and is the head most published skin-shading
#         work is shown on - good for a quick A/B.
#
#   face  The MPFB / MakeHuman example avatar (CC0, ~35 MB) from TalkingHead.
#         Lower fidelity, but it is the only one of the three with SEPARATE
#         eyeball, teeth and tongue meshes - which is what exercises the eye
#         refraction and the tooth/gum/saliva materials at all.
#         KNOWN ISSUE: this avatar carries morph targets and the viewer's
#         morph-instance path currently hangs on it and presents a corrupt
#         frame. Reproduced on a clean tree, so it predates the character work;
#         the look-dev bench will not auto-pick it. Until it is fixed, the eye
#         and mouth materials are exercised by the bench's procedural stand-in
#         (run --demo lookdev with no head asset present).
#
# Then:
#   build/linux/runtime/rx --demo lookdev
# (the lab picks up assets/head/head.glb on its own; RX_LOOKDEV_SUBJECT=<path>
# overrides it).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/assets/head"
WORK="$DEST/.work"
mkdir -p "$DEST"

want="${1:-scan}"

fetch_scan() {
  local url="https://renderpeople.com/sample/free/rp_dennis_posed_004_OBJ.zip"
  mkdir -p "$WORK"
  if [ ! -s "$WORK/dennis.zip" ]; then
    echo "fetching the free RenderPeople scan (~133 MB)..."
    curl -fL --progress-bar "$url" -o "$WORK/dennis.zip"
  fi
  echo "unpacking..."
  ( cd "$WORK" && unzip -o -q dennis.zip )
  echo "converting to glb..."
  python3 "$REPO_DIR/tools/obj_to_glb.py" \
    "$WORK/rp_dennis_posed_004_100k.obj" "$DEST/head.glb" \
    --basecolor "$WORK/tex/rp_dennis_posed_004_dif_8k.jpg" \
    --normal "$WORK/tex/rp_dennis_posed_004_norm_8k.jpg" \
    --scale 0.01 --recenter --yaw 180 --roughness 0.45 --name skin
  # The scan is a single unnamed material, so the lab's name-based region guess
  # cannot find eyes or teeth on it. Naming it `skin` is the truthful answer:
  # the eye and mouth materials are exercised by `face` and by the procedural
  # stand-in, not by a clothed body scan.
  echo "done: $DEST/head.glb"
  echo "  (the per-material spec maps are left in $WORK/tex if you want to"
  echo "   author an ORM map; the lab fits roughness live without one)"
}

fetch_head() {
  local base="https://raw.githubusercontent.com/mrdoob/three.js/dev/examples/models/gltf/LeePerrySmith"
  mkdir -p "$WORK/lps"
  for f in LeePerrySmith.glb Map-COL.jpg Infinite-Level_02_Tangent_SmoothUV.jpg Map-SPEC.jpg; do
    [ -s "$WORK/lps/$f" ] || curl -fsSL "$base/$f" -o "$WORK/lps/$f"
  done
  python3 "$REPO_DIR/tools/wire_gltf_textures.py" \
    "$WORK/lps/LeePerrySmith.glb" "$DEST/lps_head.glb" \
    --basecolor "$WORK/lps/Map-COL.jpg" \
    --normal "$WORK/lps/Infinite-Level_02_Tangent_SmoothUV.jpg" \
    --scale 0.0257 --yaw 180 --name skin
  echo "done: $DEST/lps_head.glb"
}

fetch_face() {
  local url="https://raw.githubusercontent.com/met4citizen/TalkingHead/main/avatars/mpfb.glb"
  [ -s "$DEST/face.glb" ] || curl -fL --progress-bar "$url" -o "$DEST/face.glb"
  echo "done: $DEST/face.glb  (separate eye / teeth / tongue meshes)"
}

case "$want" in
  scan) fetch_scan ;;
  head) fetch_head ;;
  face) fetch_face ;;
  all)  fetch_scan; fetch_head; fetch_face ;;
  *) echo "unknown subject: $want (scan|head|face|all)" >&2; exit 1 ;;
esac
