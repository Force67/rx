#include "rhi_bindings.hlsli"
// Imposter bake vertex: mesh position/normal/color through an ortho
// view-projection for one octahedral cell.
struct BakePush {
  column_major float4x4 view_proj;
};
PUSH_CONSTANTS(BakePush, pc);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float4 tangent : TANGENT;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
  [[vk::location(4)]] float4 color : COLOR0;
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 color : COLOR0;
};

VsOut main(VsIn input) {
  VsOut o;
  o.pos = mul(pc.view_proj, float4(input.position, 1.0));
  o.normal = input.normal;
  o.color = input.color;
  return o;
}
