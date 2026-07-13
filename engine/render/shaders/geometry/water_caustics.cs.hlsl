// Underwater caustics + wave shadows. Refracts a grid of sun rays through the
// water surface (FFT displacement/normal maps when active, an analytic Gerstner
// field otherwise) onto a reference receiver plane a fixed depth below the rest
// height, and writes a tiling world-space caustic map (RG16F):
//   R = caustic density, energy-conserving. Every surface photon carries unit
//       energy and is bilinearly splatted into the texel it lands on, so the
//       map sums to the photon count and its MEAN is 1: convergent refraction
//       piles photons up (R>1, brighter) and divergent refraction thins them
//       out (R<1, darker). No energy is created - brightening is paid for by
//       darkening elsewhere.
//   G = a soft wave-shadow term: the sun's Fresnel transmission through the
//       surface above the texel, so the backs of waves let less light through.
//
// Three phases over the 512x512 grid, driven by control.x:
//   0 clear the fixed-point accumulation buffer
//   1 scatter: one thread per surface photon, InterlockedAdd into the buffer
//   2 resolve: normalize to the mean and write the RG16F map + wave shadow
//
// The map tiles over kTile metres. With the FFT ocean (periodic over its patch
// size) this tiles seamlessly; the Gerstner field is near-periodic at this
// scale and the residual seam is buried by the depth fade in the sampler.

#include "rhi_bindings.hlsli"
#include "water_waves.hlsli"  // GerstnerWave + constants (FFT path uses our own bound maps)

struct PushData {
  float4 sun;      // xyz sun travel direction (normalized, y<0), w time (s)
  float4 params;   // x tile size (m), y rest height (m), z receiver depth (m), w eta (n_air/n_water)
  float4 misc;     // x fixed-point scale, y fft flag, zw unused
  uint4 control;   // x phase, yzw unused
};
PUSH_CONSTANTS(PushData, push);

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> accum : register(u0, space0);
[[vk::binding(1, 0)]] RWTexture2D<float2> caustic_out : register(u1, space0);
// FFT ocean maps (env-owned, GENERAL layout), bound here as our own inputs so
// this pass does not depend on the mesh env set. 1x1 dummies when the ocean is
// off; the Gerstner branch never samples them.
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float4> ocean_disp : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState ocean_disp_sampler : register(s2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float4> ocean_norm : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState ocean_norm_sampler : register(s3, space0);

static const uint kRes = 512u;   // mirrors WaterCaustics::kSize; power of two for wrap masking
static const uint kMask = kRes - 1u;

// Fine capillary ripple detail below the Gerstner/FFT resolution. The coarse
// swell barely bends the sun over a shallow receiver; it is these sub-metre
// ripples that focus the light into the characteristic caustic web. Two-plus
// octaves of gradient noise, scrolled in time so the pattern animates.
float2 CausticHash(float2 p) {
  float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yzx + 33.33);
  return frac((q.xx + q.yz) * q.zy) * 2.0 - 1.0;
}
float CausticNoise(float2 p) {
  float2 i = floor(p);
  float2 f = frac(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  float a = dot(CausticHash(i), f);
  float b = dot(CausticHash(i + float2(1, 0)), f - float2(1, 0));
  float c = dot(CausticHash(i + float2(0, 1)), f - float2(0, 1));
  float d = dot(CausticHash(i + float2(1, 1)), f - float2(1, 1));
  return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float RippleHeight(float2 p, float t) {
  float h = 0.0;
  float amp = 1.0;
  float freq = 1.0;
  float2 drift = float2(0.35, 0.21);
  [unroll]
  for (int i = 0; i < 3; ++i) {
    h += amp * CausticNoise(p * freq + drift * t * freq);
    amp *= 0.5;
    freq *= 2.3;
    drift = float2(-drift.y, drift.x) * 1.2;
  }
  return h;
}
float3 RippleNormal(float2 p, float t, float strength) {
  const float e = 0.15;
  float h0 = RippleHeight(p, t);
  float hx = RippleHeight(p + float2(e, 0), t);
  float hz = RippleHeight(p + float2(0, e), t);
  return normalize(float3(-(hx - h0) / e * strength, 1.0, -(hz - h0) / e * strength));
}

// Surface normal + height at a rest xz. FFT samples the maps; Gerstner evaluates
// the analytic field. Height is the vertical displacement above the rest plane.
// The coarse normal is refined with fine ripple detail so refraction focuses.
void SurfaceAt(float2 xz, float t, bool fft, out float3 n, out float h) {
  if (fft) {
    float2 uv = xz / kOceanPatchSize;
    n = normalize(ocean_norm.SampleLevel(ocean_norm_sampler, uv, 0.0).xyz);
    h = ocean_disp.SampleLevel(ocean_disp_sampler, uv, 0.0).y;
  } else {
    float crest;
    float3 off = GerstnerWave(xz, t, n, crest);
    h = off.y;
  }
  float3 rn = RippleNormal(xz * 2.6, t * 0.7, 0.45);
  n = normalize(float3(n.xz + rn.xz, n.y).xzy);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= kRes || id.y >= kRes) return;
  uint lin_idx = id.y * kRes + id.x;
  uint phase = push.control.x;

  if (phase == 0u) {  // clear the accumulation buffer
    accum[lin_idx] = 0u;
    return;
  }

  float tile = push.params.x;
  float texel = tile / float(kRes);
  float t = push.sun.w;
  bool fft = push.misc.y > 0.5;

  if (phase == 1u) {  // scatter one photon per surface texel
    float2 xz = (float2(id.xy) + 0.5) * texel;
    float3 n;
    float h;
    SurfaceAt(xz, t, fft, n, h);
    float3 origin = float3(xz.x, push.params.y + h, xz.y);
    float3 refr = refract(normalize(push.sun.xyz), n, push.params.w);
    if (dot(refr, refr) < 1e-6 || refr.y >= -1e-4) return;  // TIR or not heading down
    float target_y = push.params.y - push.params.z;
    float march = (target_y - origin.y) / refr.y;
    float2 hit = origin.xz + refr.xz * march;
    // Bilinear splat into the wrapped tile so the density field is smooth; the
    // four weights sum to 1 so each photon deposits exactly one unit of energy.
    float2 tp = frac(hit / tile) * float(kRes) - 0.5;
    int2 base = int2(floor(tp));
    float2 f = tp - float2(base);
    float scale = push.misc.x;
    [unroll]
    for (int oy = 0; oy < 2; ++oy) {
      [unroll]
      for (int ox = 0; ox < 2; ++ox) {
        uint2 p = uint2((base + int2(ox, oy)) & int2(kMask, kMask));
        float w = (ox == 0 ? 1.0 - f.x : f.x) * (oy == 0 ? 1.0 - f.y : f.y);
        uint add = (uint)(w * scale + 0.5);
        if (add > 0u) InterlockedAdd(accum[p.y * kRes + p.x], add);
      }
    }
    return;
  }

  // phase 2: resolve. Photon count == texel count, each deposits unit energy, so
  // the map already has mean 1; just undo the fixed-point scale.
  float density = float(accum[lin_idx]) / max(push.misc.x, 1.0);

  // Wave shadow: Fresnel transmission of the sun through the surface above this
  // texel. Grazing (steep) surface -> more reflected, less transmitted -> darker.
  float2 xz = (float2(id.xy) + 0.5) * texel;
  float3 n;
  float h;
  SurfaceAt(xz, t, fft, n, h);
  float ndl = saturate(dot(n, -normalize(push.sun.xyz)));
  float fresnel = 0.02 + 0.98 * pow(1.0 - ndl, 5.0);
  float shadow = saturate(1.0 - fresnel);

  caustic_out[id.xy] = float2(density, shadow);
}
