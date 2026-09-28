#include "rhi_bindings.hlsli"
// Editor debug lines: world-space line-list vertices projected by the camera
// view-projection. Colour is a per-vertex rgba8 the pixel shader passes through.
struct PushData {
  column_major float4x4 view_proj;
};
PUSH_CONSTANTS(PushData, push);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float4 color : COLOR;
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float4 color : COLOR;
};

VsOut main(VsIn input) {
  VsOut o;
  o.pos = mul(push.view_proj, float4(input.position, 1.0));
  o.color = input.color;
  return o;
}
