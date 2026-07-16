#include "rhi_bindings.hlsli"
// Surface weather: how precipitation marks the world, applied to the lit scene.
// Rain wets surfaces (darkens them as water fills pores, and adds a glossy sky
// reflection on up-facing puddles); snow settles white on up-facing surfaces.
// Wetness and snow cover are independent channels, so slush (melting snow over
// wet ground) works. The top-down sky-occlusion map gates both: nothing
// accumulates under bridges or roofs, with a softly dilated drip line. Ripple
// rings run only while rain is actually falling. A screen-space pass over the
// G-buffer normals + depth.

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D color_in : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);  // world-space, octahedral
[[vk::binding(3, 0)]] Texture2D depth_in : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] TextureCube sky : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState sky_sampler : register(s4, space0);
// Top-down sky-occlusion depth (see precip_occlusion.h for the transform).
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2D<float> occlusion_map : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState occlusion_sampler : register(s5, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;  // xyz eye
  float4 params;      // x wetness 0..1, y snow cover 0..1, z time s, w live rain 0..1
  float4 occl;        // sky-occlusion: center xz, 1/half extent, top_y
  float4 occl2;       // x y-range (<= 0 disables the gating)
  uint2 size;
  uint2 pad;
};
PUSH_CONSTANTS(PushData, pc);

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

float Hash21(float2 p) {
  p = frac(p * float2(123.34, 456.21));
  p += dot(p, p + 45.32);
  return frac(p.x * p.y);
}

// Two octaves of bilinear value noise; enough for a puddle mask.
float ValueNoise(float2 p) {
  float2 cell = floor(p);
  float2 f = frac(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = Hash21(cell);
  float b = Hash21(cell + float2(1, 0));
  float c = Hash21(cell + float2(0, 1));
  float d = Hash21(cell + float2(1, 1));
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Expanding-ring raindrop ripples on a horizontal water film, anchored to world
// XZ so they sit on the ground instead of swimming with the camera. A 3x3 grid
// of drop cells, each spawning a ring that grows and fades over its lifetime.
// rate scales spawn density with the live rain, not the standing wetness.
float Ripples(float2 wpos, float time, float rate) {
  float2 grid = wpos * 3.0;  // cells per metre -> ripple density
  float2 cell = floor(grid);
  float2 f = frac(grid);
  float acc = 0.0;
  [unroll]
  for (int j = -1; j <= 1; ++j) {
    [unroll]
    for (int i = -1; i <= 1; ++i) {
      float2 o = float2(i, j);
      float2 c = cell + o;
      float h = Hash21(c);
      // Light rain leaves most cells dry this cycle.
      if (h > rate) continue;
      float2 dpos = o + float2(Hash21(c + 3.1), Hash21(c + 7.7));  // jitter in cell
      float life = frac(time * (0.7 + 0.5 * rate) + h);            // 0..1 ring age
      float d = length(f - dpos);
      float ring = exp(-pow((d - life * 0.7) * 14.0, 2.0));        // thin ring at radius
      acc += ring * (1.0 - life);                                  // fade as it ages
    }
  }
  return saturate(acc);
}

// Sky visibility from the top-down occlusion map: 4 dilated taps with a soft
// height compare, so the dry strip under a bridge feathers out over ~a metre
// instead of a hard texel edge.
float SkyVisibility(float3 world) {
  if (pc.occl2.x <= 0.0) return 1.0;
  float2 base_uv = (world.xz - pc.occl.xy) * pc.occl.z * 0.5 + 0.5;
  // The map only covers ~100 m around the camera; beyond it the clamped
  // sampler would smear the border texel across the world, cutting dry or
  // snow-free bands into distant terrain. Outside the map, assume open sky.
  if (any(base_uv < 0.0) || any(base_uv > 1.0)) return 1.0;
  const float texel = 1.0 / 512.0;
  const float2 taps[4] = {float2(-1.2, -0.4), float2(1.2, 0.4),
                          float2(-0.4, 1.2), float2(0.4, -1.2)};
  float vis = 0.0;
  [unroll]
  for (int i = 0; i < 4; ++i) {
    float d = occlusion_map.SampleLevel(occlusion_sampler, base_uv + taps[i] * texel, 0.0);
    float occ_y = pc.occl.w - d * pc.occl2.x;
    // The shaded surface is itself in the map, so being AT the occluder height
    // means it IS the top surface (wet roof); well below it means covered.
    vis += saturate((world.y - occ_y + 0.9) / 1.2);
  }
  return vis * 0.25;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int3 p = int3(id.xy, 0);
  float3 color = color_in.Load(p).rgb;
  float depth = depth_in.Load(p).r;
  if (depth <= 0.0) {  // sky: nothing to wet
    out_image[id.xy] = float4(color, 1.0);
    return;
  }

  float3 n = OctDecode(normal_map.Load(p).rg);
  float up = saturate(n.y);  // horizontal surfaces collect water / snow

  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  float4 wp = mul(pc.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  float3 world = wp.xyz / wp.w;
  float3 view = normalize(world - pc.camera_pos.xyz);

  float sky_vis = SkyVisibility(world);
  float3 result = color;

  // Rain: darken, then add a glossy reflection of the sky (puddle sheen).
  // The sheen concentrates in a world-anchored puddle mask - low ground picks
  // up mirror pools while the asphalt between them just darkens - instead of
  // glazing the whole floor into a uniform milky sheet.
  float wet = pc.params.x * (0.30 + 0.70 * up) * sky_vis;
  if (wet > 0.0) {
    result *= lerp(1.0, 0.55, wet);
    float2 pw = world.xz * 0.21;
    float pn = ValueNoise(pw) * 0.65 + ValueNoise(pw * 2.7 + 13.7) * 0.35;
    // Puddles spread as wetness rises; ~soaked ground is mostly pooled.
    float puddle = smoothstep(0.62 - 0.22 * pc.params.x, 0.85, pn);
    float3 refl = reflect(view, n);
    float3 env = sky.SampleLevel(sky_sampler, refl, 0).rgb;
    float fresnel = pow(saturate(1.0 - dot(-view, n)), 4.0);
    result += env * (0.03 + 0.5 * fresnel) * wet * up * (0.12 + 0.88 * puddle);
    // Raindrops dimpling the puddle: only while it actually rains (a soaked
    // street after the shower is still and mirror-like), fading out beyond a
    // few metres where the rings would alias.
    float rain = pc.params.w;
    if (rain > 0.0) {
      float dist = length(world - pc.camera_pos.xyz);
      float rip = Ripples(world.xz, pc.params.z, rain) * up * wet *
                  saturate(1.0 - dist / 25.0);
      result += env * rip * (0.15 + 0.5 * puddle);
    }
  }

  // Snow: settles white on up-facing surfaces. A smooth coverage curve keeps
  // gentle slopes partially dusted instead of snapping bare at a threshold.
  float cover = pc.params.y * smoothstep(0.15, 0.95, up * 0.85 + pc.params.y * 0.25) * sky_vis;
  if (cover > 0.0) {
    result = lerp(result, float3(0.88, 0.91, 0.98), cover);
    // View-dependent sparkle: world-anchored glitter cells whose facet phase
    // depends on the view direction, so crystals flash as the camera moves.
    float2 cell = floor(world.xz * 90.0);
    float gh = Hash21(cell);
    float facet = frac(gh * 61.7 + dot(view, float3(13.1, 17.7, 19.3)) * 3.7);
    float glint = smoothstep(0.985, 1.0, facet) * cover;
    float3 env_up = sky.SampleLevel(sky_sampler, float3(0, 1, 0), 0).rgb;
    result += (env_up + 0.6) * glint * 2.5;
  }

  out_image[id.xy] = float4(result, 1.0);
}
