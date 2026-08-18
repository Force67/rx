#!/usr/bin/env bash
# Downloads NVIDIA's public OpenUSD sample scenes into assets/usd/.
# Usage: tools/get_usd_samples.sh [scene...]   (default: templates)
#   templates  Assets/Scenes/Templates  ~5 MB    small studio/interior stages
#   attic      Samples/OldAttic         ~1.6 GB  Attic_NVIDIA.usd
#   astronaut  Samples/Astronaut        ~530 MB
#   marbles    Samples/Marbles          ~1.1 GB
# Then: ./build.sh run -- --usd assets/usd/attic/Attic_NVIDIA.usd
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_ROOT="$REPO_DIR/assets/usd"
BUCKET="https://omniverse-content-production.s3.us-west-2.amazonaws.com"
JOBS="${JOBS:-8}"

prefix_for() {
  case "$1" in
    templates) echo "Assets/Scenes/Templates/" ;;
    attic)     echo "Samples/OldAttic/" ;;
    astronaut) echo "Samples/Astronaut/" ;;
    marbles)   echo "Samples/Marbles/" ;;
    *) echo "unknown scene: $1 (templates|attic|astronaut|marbles)" >&2; return 1 ;;
  esac
}

# S3 ListObjectsV2 pages at 1000 keys; follow the continuation token.
list_keys() {
  local prefix="$1" token="" url page
  while :; do
    url="$BUCKET/?list-type=2&prefix=$prefix&max-keys=1000"
    [ -n "$token" ] && url="$url&continuation-token=$token"
    page=$(curl -sfL "$url")
    printf '%s' "$page" | grep -o '<Key>[^<]*</Key>' | sed 's/<[^>]*>//g'
    printf '%s' "$page" | grep -q '<IsTruncated>true</IsTruncated>' || break
    token=$(printf '%s' "$page" | grep -o '<NextContinuationToken>[^<]*</NextContinuationToken>' |
      sed 's/<[^>]*>//g' | sed 's/+/%2B/g; s|/|%2F|g; s/=/%3D/g')
    [ -n "$token" ] || break
  done
}

fetch_one() {
  local key="$1" dest="$2" prefix="$3" rel out
  rel="${key#"$prefix"}"
  out="$dest/$rel"
  # Directory-marker keys end in / and carry no payload.
  case "$rel" in ""|*/) return 0 ;; esac
  [ -s "$out" ] && return 0
  mkdir -p "$(dirname "$out")"
  # Some keys contain spaces, which curl will not accept raw in a URL.
  curl -sfL --retry 3 "$BUCKET/$(printf '%s' "$key" | sed 's/ /%20/g')" -o "$out.part" ||
    { echo "  failed: $rel" >&2; rm -f "$out.part"; return 0; }
  mv "$out.part" "$out"
}
export -f fetch_one
export BUCKET

for scene in "${@:-templates}"; do
  prefix="$(prefix_for "$scene")"
  dest="$DEST_ROOT/$scene"
  echo "listing $prefix ..."
  keys="$(list_keys "$prefix")"
  total=$(printf '%s\n' "$keys" | grep -c . || true)
  [ "$total" -gt 0 ] || { echo "no objects under $prefix" >&2; exit 1; }
  echo "fetching $total objects into $dest (JOBS=$JOBS)"
  printf '%s\n' "$keys" |
    xargs -d '\n' -P "$JOBS" -I{} bash -c 'fetch_one "$@"' _ {} "$dest" "$prefix"
  echo "done: $dest"
  find "$dest" -maxdepth 1 -iname '*.usd*' -printf '  %p\n'
done
