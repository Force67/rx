#include "rhi_bindings.hlsli"
#include "gi/material_class.hlsli"
// Bilateral upscale of the half-res reflection trace (RX_REFL_HALF) to full
// render resolution before NRD REBLUR_SPECULAR consumes it. The trace runs at
// half res (quarter the rays -- the dominant reflection cost); this reconstructs
// the full-res packed radiance+normHitDist with depth/normal-aware weights so
// reflections do not bleed across silhouettes before the denoiser locks them in.
// Mirrors the RCGI upscale weighting pattern (rcgi_upscale.cs.hlsl).

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_full : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> in_half : register(t1, space0);  // packed radiance + normHitDist
[[vk::binding(2, 0)]] Texture2D<float> depth_map : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> normal_map : register(t3, space0);

struct PushData {
  uint4 dims;    // full_w, full_h, half_w, half_h
  float4 params; // near_plane, ...
};
PUSH_CONSTANTS(PushData, push);

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 s = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * s;
  }
  return normalize(d);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 full = push.dims.xy;
  if (id.x >= full.x || id.y >= full.y) return;

  float depth = depth_map.Load(int3(id.xy, 0));
  if (depth <= 0.0) {  // sky: no reflection
    out_full[id.xy] = 0.0.xxxx;
    return;
  }
  float near = push.params.x;
  float cz = near / depth;
  float4 cnr = normal_map.Load(int3(id.xy, 0));
  float3 cn = OctDecode(cnr.rg);
  float c_class = cnr.a;  // material class (item 22a): reject cross-class taps

  uint2 hsz = push.dims.zw;
  float2 uv = (float2(id.xy) + 0.5) / float2(full);
  float2 hpix = uv * float2(hsz) - 0.5;
  int2 base = int2(floor(hpix));
  float2 fr = hpix - float2(base);
  const int2 taps[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
  float bw[4] = {(1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y,
                 fr.x * fr.y};

  float4 sum = 0.0.xxxx;
  float wsum = 0.0;
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    int2 q = clamp(base + taps[i], int2(0, 0), int2(hsz) - 1);
    // reconstruct the half-res tap's full-res representative for the guide test
    float2 quv = (float2(q) + 0.5) / float2(hsz);
    int2 qfp = clamp(int2(quv * float2(full)), int2(0, 0), int2(full) - 1);
    float qd = depth_map.Load(int3(qfp, 0));
    if (qd <= 0.0) continue;
    float4 qnr = normal_map.Load(int3(qfp, 0));
    float qz = near / qd;
    float3 qn = OctDecode(qnr.rg);
    float wz = exp(-abs(cz - qz) / max(cz, 0.1) * 8.0);
    float wn = pow(saturate(dot(cn, qn)), 8.0);
    float wc = RxMatClassMatch(c_class, qnr.a);  // reject cross-class neighbours
    float w = max(bw[i], 1e-3) * wz * wn * wc;
    sum += in_half.Load(int3(q, 0)) * w;
    wsum += w;
  }
  // No valid tap: nearest-neighbour fallback so we never emit garbage.
  if (wsum <= 1e-4) {
    int2 nn = clamp(int2(round(hpix)), int2(0, 0), int2(hsz) - 1);
    out_full[id.xy] = in_half.Load(int3(nn, 0));
    return;
  }
  out_full[id.xy] = sum / wsum;
}
