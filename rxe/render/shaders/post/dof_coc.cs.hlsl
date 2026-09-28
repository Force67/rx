#include "rhi_bindings.hlsli"
// Depth of field: circle of confusion from the resolved focus distance.
// CoC is signed (negative = near field)
// in output pixels, clamped to max_coc.
struct CocPush {
  uint2 size;
  float near_plane;
  float aperture;       // coc pixels per unit of defocus, the "f-stop" feel
  float max_coc;        // pixels
  float3 pad;
};
PUSH_CONSTANTS(CocPush, pc);

[[vk::image_format("r16f")]] [[vk::binding(0, 0)]] RWTexture2D<float> coc_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float> focus_state : register(t2, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;

  float focus = focus_state.Load(int3(0, 0, 0));
  uint width, height;
  depth_map.GetDimensions(width, height);
  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  uint2 p = min(uint2(uv * float2(width, height)), uint2(width, height) - 1u);
  float depth = depth_map.Load(int3(p, 0));
  float z = depth > 0.0 ? pc.near_plane / depth : 1e6;
  // Thin lens: coc grows with relative defocus; signed so near blur bleeds.
  float coc = pc.aperture * (z - focus) / max(z, 1e-3);
  coc_out[id.xy] = clamp(coc, -pc.max_coc, pc.max_coc);
}
