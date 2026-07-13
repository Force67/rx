#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
// RCGI blend. Accumulates the current cascade's probe rays into its octahedral
// irradiance (mode 0) or visibility-moment (mode 1) slab with hysteresis. Hit
// rays fetch their radiance by re-hashing the hit point into the world radiance
// cache; miss rays carry sky radiance in the rays buffer; backface rays only
// shorten visibility. One thread per interior texel of the cascade slab.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> atlas : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D rays : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState rays_sampler : register(s1, space0);
[[vk::binding(2, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> rcgi_state : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<uint2> rcgi_radiance : register(t4, space0);

struct PushData {
  float4 rotation_x;
  float4 rotation_y;
  float4 rotation_z;
  uint mode;   // 0 irradiance, 1 visibility
  uint reset;  // 1 = ignore history (post-snap reconverge)
  uint2 pad;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint texels = push.mode == 0u ? kRcgiIrrTexels : kRcgiVisTexels;
  uint stride = texels + 2u;
  uint probes_x = kRcgiProbesPerAxis * kRcgiProbesPerAxis;  // x * z
  uint slab_h = stride * kRcgiProbesPerAxis;
  if (id.x >= probes_x * stride || id.y >= slab_h) return;

  uint2 in_probe = uint2(id.x % stride, id.y % stride);
  if (in_probe.x == 0u || in_probe.y == 0u || in_probe.x > texels || in_probe.y > texels) return;

  uint cascade = rcgi.misc.x;
  uint ray_count = (uint)rcgi.sun_color.w;
  uint2 probe_xy = uint2(id.x / stride, id.y / stride);
  uint3 probe = uint3(probe_xy.x % kRcgiProbesPerAxis, probe_xy.y,
                      probe_xy.x / kRcgiProbesPerAxis);
  uint probe_index = RcgiProbeIndex(probe);
  float3 origin = RcgiProbePosition(rcgi, cascade, probe);

  float2 oct = (float2(in_probe) - 1.0 + 0.5) / float(texels) * 2.0 - 1.0;
  float3 texel_dir = RcgiOctDecode(oct);

  float4 sum = 0.0.xxxx;
  float weight_sum = 0.0;
  for (uint r = 0u; r < ray_count; ++r) {
    float3 fib = RcgiFibonacci(r, ray_count);
    float3 dir = normalize(float3(dot(push.rotation_x.xyz, fib), dot(push.rotation_y.xyz, fib),
                                  dot(push.rotation_z.xyz, fib)));
    float4 ray = rays.Load(int3(r, probe_index, 0));
    float hitw = ray.a;

    if (push.mode == 0u) {
      if (hitw == 0.0) continue;  // backface: no light
      float weight = max(dot(texel_dir, dir), 0.0);
      if (weight <= 1e-4) continue;
      float3 radiance;
      if (hitw < 0.0) {
        radiance = ray.rgb;  // miss: sky
      } else {
        float3 hit_pos = origin + dir * hitw;
        if (!RcgiCacheLookup(rcgi, rcgi_state, rcgi_radiance, hit_pos, radiance)) radiance = 0.0.xxx;
      }
      sum.rgb += radiance * weight;
      weight_sum += weight;
    } else {
      float weight = pow(max(dot(texel_dir, dir), 0.0), 64.0);
      float dist = min(abs(hitw), rcgi.params.x);
      sum.rg += float2(dist, dist * dist) * weight;
      weight_sum += weight;
    }
  }

  float4 result;
  if (push.mode == 0u) {
    float3 irr = weight_sum > 1e-4 ? sum.rgb / weight_sum : 0.0.xxx;
    result = float4(sqrt(irr), 1.0);  // perceptual encoding against banding
  } else {
    float2 moments = weight_sum > 1e-4
                         ? sum.rg / weight_sum
                         : float2(rcgi.params.x, rcgi.params.x * rcgi.params.x);
    result = float4(moments, 0.0, 1.0);
  }

  uint2 texel = uint2(id.x, cascade * slab_h + id.y);
  if (push.reset == 0u) result = lerp(result, atlas[texel], rcgi.params.y);
  atlas[texel] = result;
}
