// Dear ImGui vertex stage for the RHI imgui render backend
// (engine/render/util/imgui_renderer.cc). Matches the standard imgui vertex
// layout: float2 position, float2 uv, RGBA8-unorm colour. The push block carries
// the ortho scale+translate the backend computes from the draw data's display
// rect, mapping imgui screen space to clip space.
#include "rhi_bindings.hlsli"

struct PushData {
  float2 scale;
  float2 translate;
};
PUSH_CONSTANTS(PushData, push);

struct VsIn {
  [[vk::location(0)]] float2 pos : POSITION;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] float4 col : COLOR0;  // fetched from an RGBA8-unorm attribute
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 col : COLOR0;
};

VsOut main(VsIn i) {
  VsOut o;
  o.pos = float4(i.pos * push.scale + push.translate, 0.0, 1.0);
  o.uv = i.uv;
  o.col = i.col;
  return o;
}
