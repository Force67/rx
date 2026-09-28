#include "rhi_bindings.hlsli"
// Editor picking: re-rasterize opaque meshes with a trivial pipeline that writes
// the per-draw entity id. The model-view-projection and the id ride in the push.
struct PushData {
  column_major float4x4 mvp;
  uint id;
};
PUSH_CONSTANTS(PushData, push);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
};

float4 main(VsIn input) : SV_Position {
  return mul(push.mvp, float4(input.position, 1.0));
}
