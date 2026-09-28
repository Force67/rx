#include "rhi_bindings.hlsli"
// Writes the per-draw entity id into the R32_UINT pick target. Background stays
// at the cleared 0 (no pickable geometry).
struct PushData {
  column_major float4x4 mvp;
  uint id;
};
PUSH_CONSTANTS(PushData, push);

uint main() : SV_Target0 {
  return push.id;
}
