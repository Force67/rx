#!/usr/bin/env bash
# Downloads the vendor vehicle test models (glTF-Binary, a few MB total) into
# assets/vehicles/ for the driving demo (--demo drive):
#   ToyCar.glb           Khronos glTF sample (CC-BY 4.0) - clearcoat paint showcase
#   CarConcept.glb       Khronos glTF sample (CC-BY 4.0) - high-end car materials
#   CesiumMilkTruck.glb  Cesium via Khronos samples (CC-BY 4.0) - classic textured truck
#   Cesium_Air.glb       CesiumGS/cesium sample data (CC-BY 4.0) - light prop aircraft
#   GroundVehicle.glb    CesiumGS/cesium sample data (CC-BY 4.0) - textured ground vehicle
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$REPO_DIR/assets/vehicles"
KHR="https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"
CESIUM="https://raw.githubusercontent.com/CesiumGS/cesium/main/Apps/SampleData/models"

mkdir -p "$DEST"
fetch() {
  local url="$1" out="$2"
  if [ -s "$DEST/$out" ]; then echo "$out (cached)"; return; fi
  echo "$out"
  curl -sfL "$url" -o "$DEST/$out"
}

fetch "$KHR/ToyCar/glTF-Binary/ToyCar.glb" ToyCar.glb
fetch "$KHR/CarConcept/glTF-Binary/CarConcept.glb" CarConcept.glb
fetch "$KHR/CesiumMilkTruck/glTF-Binary/CesiumMilkTruck.glb" CesiumMilkTruck.glb
fetch "$CESIUM/CesiumAir/Cesium_Air.glb" Cesium_Air.glb
fetch "$CESIUM/GroundVehicle/GroundVehicle.glb" GroundVehicle.glb
echo "done: $DEST"
