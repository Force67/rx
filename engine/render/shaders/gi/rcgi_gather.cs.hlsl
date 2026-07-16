#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
#include "gi/sh.hlsli"
// RCGI M2 final gather (half render resolution). One cosine-weighted ray per
// gather pixel around the g-buffer normal, blue-noise rotated per frame so the
// temporal filter integrates. ZERO shading at the hit: radiance comes from a
// three-level cache fallback (previous-frame lit screen colour -> world radiance
// cache -> irradiance cascades; sky on a ray miss). The ray's radiance is
// projected into 2-band SH along the ray direction; the SH triple + a hit
// distance feed the denoise / upscale chain. See RCGI.md section 4.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> sh_r_out : register(u0, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]] RWTexture2D<float4> sh_g_out : register(u1, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(2, 0)]] RWTexture2D<float4> sh_b_out : register(u2, space0);
[[vk::image_format("r16f")]] [[vk::binding(3, 0)]] RWTexture2D<float> hit_t_out : register(u3, space0);
[[vk::binding(4, 0)]] Texture2D<float> depth_map : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float4> normal_map : register(t5, space0);
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas : register(t6, space0);
[[vk::binding(7, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b7, space0);
[[vk::binding(8, 0)]] StructuredBuffer<uint> rcgi_state : register(t8, space0);
[[vk::binding(9, 0)]] StructuredBuffer<uint2> rcgi_radiance : register(t9, space0);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]] Texture2D irr_atlas : register(t10, space0);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]] SamplerState irr_sampler : register(s10, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] Texture2D vis_atlas : register(t11, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] SamplerState vis_sampler : register(s11, space0);
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]] TextureCube sky : register(t12, space0);
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]] SamplerState sky_sampler : register(s12, space0);
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]] Texture2D prev_color : register(t13, space0);
[[vk::combinedImageSampler]] [[vk::binding(13, 0)]] SamplerState prev_color_sampler : register(s13, space0);
[[vk::combinedImageSampler]] [[vk::binding(14, 0)]] Texture2D<float> prev_depth : register(t14, space0);
[[vk::combinedImageSampler]] [[vk::binding(14, 0)]] SamplerState prev_depth_sampler : register(s14, space0);
// Phase 3: probe relocation metadata + interior volumes (irradiance fallback).
[[vk::binding(15, 0)]] StructuredBuffer<uint2> probe_meta : register(t15, space0);
[[vk::binding(16, 0)]] StructuredBuffer<float4> interior_vols : register(t16, space0);

struct PushData {
  column_major float4x4 inv_view_proj;   // current, unjittered
  column_major float4x4 prev_view_proj;  // last frame, unjittered
  float4 camera_pos;                     // xyz eye, w near plane
  uint4 dims;                            // full_w, full_h, gather_w, gather_h
  uint4 misc;                            // frame_index, screen_valid, asuint(ray_max), pad
};
PUSH_CONSTANTS(PushData, push);

// Hash-based per-pixel, per-frame 2D noise (decorrelated frame to frame).
float2 Hash2(uint2 p, uint frame) {
  uint s = p.x * 3266489917u + p.y * 668265263u + frame * 374761393u;
  s = (s ^ (s >> 15)) * 2246822519u;
  s ^= s >> 13; s *= 3266489917u; s ^= s >> 16;
  uint t = s * 2654435761u;
  return float2(s & 0xffffffu, t & 0xffffffu) / 16777215.0;
}

void OrthoBasis(float3 n, out float3 t, out float3 b) {
  float s = n.z >= 0.0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float d = n.x * n.y * a;
  t = float3(1.0 + s * n.x * n.x * a, s * d, -s * n.x);
  b = float3(d, s + n.y * n.y * a, -n.y);
}

float3 WorldFromDepth(float2 uv, float depth) {
  float4 world = mul(push.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  return world.xyz / world.w;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 gather_size = push.dims.zw;
  if (id.x >= gather_size.x || id.y >= gather_size.y) return;

  // Zero SH by default so sky / disoccluded gather pixels are well defined.
  sh_r_out[id.xy] = 0.0.xxxx;
  sh_g_out[id.xy] = 0.0.xxxx;
  sh_b_out[id.xy] = 0.0.xxxx;
  hit_t_out[id.xy] = push.dims.x * 4.0;  // large = "miss" for the denoise weights

  uint2 full_size = push.dims.xy;
  float2 uv = (float2(id.xy) + 0.5) / float2(gather_size);
  int2 fp = clamp(int2(uv * float2(full_size)), int2(0, 0), int2(full_size) - 1);

  float depth = depth_map.Load(int3(fp, 0));
  if (depth <= 0.0) return;  // sky pixel: leave zeroed

  float3 pos = WorldFromDepth(uv, depth);
  float3 n = RcgiOctDecode(normal_map.Load(int3(fp, 0)).xy);

  uint frame = push.misc.x;
  float ray_max = asfloat(push.misc.z);

  // Cosine-weighted hemisphere direction around n.
  float2 u = Hash2(id.xy, frame);
  float3 t, b;
  OrthoBasis(n, t, b);
  float r = sqrt(u.x);
  float phi = 6.2831853 * u.y;
  float3 local = float3(r * cos(phi), r * sin(phi), sqrt(max(0.0, 1.0 - u.x)));
  float3 dir = normalize(t * local.x + b * local.y + n * local.z);

  float dist_cam = length(pos - push.camera_pos.xyz);
  float origin_bias = max(0.01, 0.002 * dist_cam);

  RayDesc ray;
  ray.Origin = pos + n * origin_bias;
  ray.TMin = 0.0;
  ray.Direction = dir;
  ray.TMax = ray_max;
  // Vegetation: cull real (non-opaque) masked geometry and hit its shrunk
  // opaque-approximation stand-in (RX_RAY_MASK_APPROX) instead.
  RayQuery<RAY_FLAG_CULL_NON_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_DIFFUSE, ray);
  rq.Proceed();

  float3 radiance;
  float hit_t;
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    // Interior mode: escaped rays sample interior ambient, not the sky (item 9a).
    radiance = RcgiSkyMiss(rcgi, sky.SampleLevel(sky_sampler, dir, 0).rgb);
    hit_t = ray_max;
  } else {
    hit_t = rq.CommittedRayT();
    float3 hit_pos = ray.Origin + dir * hit_t;
    bool resolved = false;
    bool used_cascade = false;  // item 11: probe AO applies only to the cascade fallback

    // (a) Screen cache: was this hit surface lit and visible last frame?
    if (push.misc.y != 0u) {
      float4 clip = mul(push.prev_view_proj, float4(hit_pos, 1.0));
      if (clip.w > 1e-4) {
        float2 puv = (clip.xy / clip.w) * 0.5 + 0.5;  // engine convention, no y-flip
        float d_expect = clip.z / clip.w;             // reversed-Z NDC depth
        if (all(puv > 0.0) && all(puv < 1.0) && d_expect > 0.0) {
          float d_stored = prev_depth.SampleLevel(prev_depth_sampler, puv, 0);
          if (d_stored > 0.0) {
            float near = push.camera_pos.w;
            float z_expect = near / d_expect;
            float z_stored = near / d_stored;
            if (abs(z_expect - z_stored) < 0.05 * max(z_expect, 0.1)) {
              radiance = prev_color.SampleLevel(prev_color_sampler, puv, 0).rgb;
              resolved = true;
            }
          }
        }
      }
    }
    // (b) World radiance cache at the hit point.
    if (!resolved) {
      float3 cached;
      if (RcgiCacheLookup(rcgi, rcgi_state, rcgi_radiance, hit_pos, cached)) {
        radiance = cached;
        resolved = true;
      }
    }
    // (c) Irradiance cascades (geometric normal approximated by -dir).
    if (!resolved) {
      radiance = SampleRcgiIrradiance(rcgi, irr_atlas, irr_sampler, vis_atlas, vis_sampler,
                                      probe_meta, interior_vols, hit_pos, -dir, -dir);
      used_cascade = true;
    }

    // Probe AO (item 11): a short ray that fell back to the sparse cascades
    // missed contact occlusion the probes cannot resolve. Attenuate by the
    // relative hit distance (their saturate(hitT/spacing*scale + bias)); the
    // screen/hash-cache hits already carry their own occlusion, so skip those.
    if (used_cascade && (rcgi.gi_flags.x & kRcgiFlagProbeAo) != 0u) {
      uint hc;
      float spacing = RcgiSelectCascade(rcgi, hit_pos, hc) ? rcgi.cascade_origin[hc].w
                                                           : rcgi.cascade_origin[0].w;
      float ao_scale = rcgi.interior.w;
      float ao_bias = asfloat(rcgi.gi_flags.z);
      radiance *= saturate(hit_t / max(spacing, 1e-3) * ao_scale + ao_bias);
    }
  }

  radiance = max(radiance, 0.0.xxx);
  radiance = min(radiance, 32.0.xxx);  // firefly clamp for temporal stability

  // Unbiased-ish SH radiance projection: cosine pdf = local.z / pi, so the
  // single-sample coefficient estimate weights by pi / cos (clamped at grazing).
  float weight = kRcgiPi / max(local.z, 0.1);
  Sh2 sh = ShZero();
  ShProjectAdd(sh, dir, radiance, weight);

  sh_r_out[id.xy] = sh.r;
  sh_g_out[id.xy] = sh.g;
  sh_b_out[id.xy] = sh.b;
  hit_t_out[id.xy] = hit_t;
}
