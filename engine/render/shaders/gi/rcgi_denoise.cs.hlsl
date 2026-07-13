#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
// RCGI M2 spatial denoise (gather resolution). Separable bilateral gaussian run
// twice (horizontal then vertical) over the SH triple. Bilateral weights come
// from linear-depth delta, normal agreement, and hit-distance delta; contact
// detail (short center hitT) is protected by sharpening the hitT falloff. See
// RCGI.md section 5.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> sh_r_out : register(u0, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]] RWTexture2D<float4> sh_g_out : register(u1, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(2, 0)]] RWTexture2D<float4> sh_b_out : register(u2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> sh_r_in : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> sh_g_in : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float4> sh_b_in : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<float> hit_t_in : register(t6, space0);
[[vk::binding(7, 0)]] Texture2D<float> depth_map : register(t7, space0);
[[vk::binding(8, 0)]] Texture2D<float4> normal_map : register(t8, space0);

struct PushData {
  uint4 dims;    // full_w, full_h, gather_w, gather_h
  uint4 misc;    // dir_x, dir_y, radius, pad
  float4 params; // near_plane, ...
};
PUSH_CONSTANTS(PushData, push);

float3 DecodeN(int2 gp) {
  uint2 full = push.dims.xy;
  uint2 gsz = push.dims.zw;
  float2 uv = (float2(gp) + 0.5) / float2(gsz);
  int2 fp = clamp(int2(uv * float2(full)), int2(0, 0), int2(full) - 1);
  return RcgiOctDecode(normal_map.Load(int3(fp, 0)).xy);
}

float LinearZ(int2 gp) {
  uint2 full = push.dims.xy;
  uint2 gsz = push.dims.zw;
  float2 uv = (float2(gp) + 0.5) / float2(gsz);
  int2 fp = clamp(int2(uv * float2(full)), int2(0, 0), int2(full) - 1);
  float d = depth_map.Load(int3(fp, 0));
  return d > 0.0 ? push.params.x / d : 1e6;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 gsz = push.dims.zw;
  if (id.x >= gsz.x || id.y >= gsz.y) return;
  int2 p = int2(id.xy);

  float cz = LinearZ(p);
  float3 cn = DecodeN(p);
  float c_hit = hit_t_in.Load(int3(p, 0));
  int2 dir = int2(push.misc.xy);
  int radius = int(push.misc.z);

  if (cz >= 1e6) {  // sky / invalid: pass through
    sh_r_out[id.xy] = sh_r_in.Load(int3(p, 0));
    sh_g_out[id.xy] = sh_g_in.Load(int3(p, 0));
    sh_b_out[id.xy] = sh_b_in.Load(int3(p, 0));
    return;
  }

  // The per-pixel single-ray hitT is itself noisy, so the hitT term must be
  // gentle (relative delta) or it rejects every neighbor and defeats the filter.
  // Contact-scale detail (short center hitT) tightens it to protect near-field
  // occlusion; distant hits filter freely.
  float contact = saturate(1.0 - c_hit);  // < 1 m: protect contact detail

  float4 acc_r = 0.0.xxxx, acc_g = 0.0.xxxx, acc_b = 0.0.xxxx;
  float wsum = 0.0;
  [loop]
  for (int i = -radius; i <= radius; ++i) {
    int2 q = p + dir * i;
    if (any(q < int2(0, 0)) || any(q >= int2(gsz))) continue;
    float qz = LinearZ(q);
    if (qz >= 1e6) continue;
    float3 qn = DecodeN(q);
    float q_hit = hit_t_in.Load(int3(q, 0));

    float wg = exp(-float(i * i) / (2.0 * float(radius * radius) * 0.25 + 1e-3));
    float wz = exp(-abs(cz - qz) / max(cz, 0.1) * 8.0);
    float wn = pow(saturate(dot(cn, qn)), 8.0);
    float rel = abs(c_hit - q_hit) / (max(c_hit, q_hit) + 0.25);
    float wh = exp(-rel * rel * (2.0 + contact * 18.0));
    float w = wg * wz * wn * wh;

    acc_r += sh_r_in.Load(int3(q, 0)) * w;
    acc_g += sh_g_in.Load(int3(q, 0)) * w;
    acc_b += sh_b_in.Load(int3(q, 0)) * w;
    wsum += w;
  }

  float inv = 1.0 / max(wsum, 1e-4);
  sh_r_out[id.xy] = acc_r * inv;
  sh_g_out[id.xy] = acc_g * inv;
  sh_b_out[id.xy] = acc_b * inv;
}
