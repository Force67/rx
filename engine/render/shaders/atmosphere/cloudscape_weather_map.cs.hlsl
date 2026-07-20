#include "rhi_bindings.hlsli"
#include "cloudscape_noise.hlsli"
// World-space weather map, regenerated whenever the weather controls change:
// 512^2 RGBA8 tiling in both axes, sampled over ~60 km of world so it reads as
// regional weather rather than a texture.
//   R = coverage    : the fraction of sky each region fills
//   G = precipitation: sparse storm cells, gated to sit under thick coverage
//   B = cloud type   : stratus .. cumulus, forced to cumulus in storm cells
//   A = 1
// Two weather states (a, b) are each evaluated and cross-faded by map_blend so
// a transition is one regenerate, not two textures. Each state's seed offsets
// the noise domain, which re-rolls the pattern while keeping it tileable (a
// constant domain shift preserves the wrapped-lattice periodicity).

[[vk::image_format("rgba8")]] [[vk::binding(0, 0)]]
RWTexture2D<float4> out_map : register(u0, space0);

struct WeatherPush {
  uint seed_a;
  float coverage_a;
  float cloud_type_a;
  float precip_a;
  uint seed_b;
  float coverage_b;
  float cloud_type_b;
  float precip_b;
  float blend;
  uint3 pad;
};
PUSH_CONSTANTS(WeatherPush, pc);

// Coverage/precip/type for one weather state at a tile.
float3 WeatherState(float2 uv, uint seed, float coverage, float cloud_type, float precip) {
  // Seed-derived domain offset: a different constant shift per state re-rolls
  // the field without disturbing the tiling.
  uint3 sq = uint3(seed, seed ^ 0x9e3779b9u, seed * 747796405u + 2891336453u);
  float2 off =
      float2(cs_hash_float(cs_hash_u32(sq ^ uint3(0x68bc21ebu, 0x02e5be93u, 0x967a889bu))),
             cs_hash_float(cs_hash_u32(sq ^ uint3(0x4f1bbcdcu, 0x5c4bcea9u, 0x9e3779b9u)))) *
      37.0;
  float2 p = uv + off;

  // Coverage: a domain-warped Perlin fbm broken into distinct formations by an
  // inverted-Worley cell field, then thresholded so the map's mean tracks the
  // coverage scalar. A threshold (not a multiply) is what turns a smooth field
  // into separated cloud masses at mid coverage.
  // Keep every domain multiplier and lattice period integral. The wrapped
  // noise is periodic over one UV tile only when uv + 1 advances an integer
  // number of wrapped lattice cells.
  float warp = cs_perlin2_fbm(p + 5.1, 1.0, 3);
  float2 wp = p + (warp - 0.5) * 0.4;
  float n = cs_perlin2_fbm(wp, 3.0, 5);
  float cells = cs_worley2_fbm(p, 3.0);
  n = saturate(n * (0.55 + 0.75 * cells));
  float thresh = 1.0 - coverage;
  float cov = saturate((n - thresh) / 0.22 + 0.5);
  cov *= smoothstep(0.0, 0.06, coverage);           // coverage 0 -> fully clear
  cov = max(cov, smoothstep(0.94, 1.0, coverage));  // coverage 1 -> fully overcast
  // Even a closed deck is never uniform: thickness keeps riding the
  // underlying field, so heavy skies hold multi-km light/dark regions (the
  // only structure that survives grazing view angles) instead of clamping
  // the whole map to 1 and rendering as one flat sheet.
  cov *= 0.72 + 0.28 * n;

  // Precipitation: sparser, larger cells; area fraction tracks the precip
  // scalar and the cells are masked to where coverage is already thick.
  float pn = cs_perlin2_fbm(p + 21.3, 1.0, 3);
  float pcells = cs_worley2_fbm(p + 8.0, 2.0);
  float precip_f = saturate((pn * pcells - (1.0 - precip)) / 0.18 + 0.35);
  precip_f *= smoothstep(0.45, 0.8, cov);
  precip_f *= smoothstep(0.0, 0.05, precip);

  // Cloud type: a broad low-frequency field around the type scalar, driven to
  // cumulus (1) inside the storm cells so precipitating decks build towers.
  float tn = cs_perlin2_fbm(p + 44.2, 1.0, 2);
  float typ = saturate(cloud_type + (tn - 0.5) * 0.5);
  typ = max(typ, precip_f);

  return float3(cov, precip_f, typ);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 dims;
  out_map.GetDimensions(dims.x, dims.y);
  if (id.x >= dims.x || id.y >= dims.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(dims);

  float3 a = WeatherState(uv, pc.seed_a, pc.coverage_a, pc.cloud_type_a, pc.precip_a);
  float3 b = a;
  bool same_state = pc.seed_a == pc.seed_b && pc.coverage_a == pc.coverage_b &&
                    pc.cloud_type_a == pc.cloud_type_b && pc.precip_a == pc.precip_b;
  if (!same_state && pc.blend > 0.0) {
    b = WeatherState(uv, pc.seed_b, pc.coverage_b, pc.cloud_type_b, pc.precip_b);
  }
  float3 rgb = lerp(a, b, saturate(pc.blend));
  out_map[id.xy] = float4(rgb, 1.0);
}
