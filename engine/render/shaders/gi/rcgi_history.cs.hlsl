#include "rhi_bindings.hlsli"
// RCGI M2 history snapshot: copy the current frame's lit HDR scene colour and
// depth into the persistent screen-cache history images the NEXT frame's final
// gather samples for its previous-frame radiance lookup. Only recorded when RCGI
// is active, so it costs nothing when off.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> color_hist : register(u0, space0);
[[vk::image_format("r32f")]] [[vk::binding(1, 0)]] RWTexture2D<float> depth_hist : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> lit : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float> depth : register(t3, space0);

struct PushData { uint2 size; uint2 pad; };
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= push.size.x || id.y >= push.size.y) return;
  color_hist[id.xy] = float4(lit.Load(int3(id.xy, 0)).rgb, 1.0);
  depth_hist[id.xy] = depth.Load(int3(id.xy, 0));
}
