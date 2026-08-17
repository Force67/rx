#include "rhi_bindings.hlsli"
// Weighted-blended order-independent transparency (McGuire & Bavoil 2013). The
// transparent geometry accumulates premultiplied colour weighted by depth into
// one target and the product of (1 - alpha) into another, so the result is
// independent of draw order. One instance per draw, parameters in the push.
struct PushData {
  column_major float4x4 model;
  float4 color;  // rgb tint, a = alpha
  float3 sun_dir;
  float pad0;
  float3 sun_color;
  float ambient;
};
PUSH_CONSTANTS(PushData, push);

// Pass constants: identical for every instance, and together more than the
// push budget, so they arrive through a uniform buffer instead.
struct WboitFrame {
  column_major float4x4 view_proj;
  float4 cluster_params;  // x slice scale, y slice bias, zw tile size px
  float4 froxel_params;   // x near, y far, z enabled
};
[[vk::binding(4, 0)]] ConstantBuffer<WboitFrame> frame : register(b4, space0);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float view_z : TEXCOORD0;  // for the depth weight
  [[vk::location(2)]] float3 world_pos : TEXCOORD1;  // for clustered lighting
};

VsOut main(VsIn input) {
  float4 world = mul(push.model, float4(input.position, 1.0));
  float4 clip = mul(frame.view_proj, world);
  VsOut o;
  o.pos = clip;
  o.normal = mul((float3x3)push.model, input.normal);
  o.view_z = clip.w;  // perspective w == view-space depth
  o.world_pos = world.xyz;
  return o;
}
