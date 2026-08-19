#include "rhi_bindings.hlsli"
#include "hair_bsdf.hlsli"
#include "geometry/hair_transmittance.hlsli"

// Strand shading through the full hair BSDF (Marschner's R / TT / TRT lobes in
// Chiang's parameterization) with Zinke dual scattering fed by the deep opacity
// map. What this replaces was a pair of Kajiya-Kay power lobes with hardcoded
// constants, no shadowing of any kind, and a flat ambient fill - which is why
// the groom read as opaque straw regardless of what colour it was given.
struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;      // xyz eye, w = ribbon width
  float4 sun;         // xyz travel direction, w intensity
  float4 sun_color;   // rgb, w = clump radius
  float4 tint;        // rgb groom tint, w = children count
};
PUSH_CONSTANTS(DrawPush, pc);

// Per-groom fibre material. Mirrors GroomMaterial in hair_strands.cc; the
// quality tier is already folded into it, so the shader never re-derives one.
struct HairMaterial {
  float3 sigma_a;
  float beta_m;
  float beta_n;
  float alpha;
  float eta;
  float density;
  float scatter_scale;
  float caps;
  float color_reference_depth;
  float pad;
};

[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float> hair_front_depth : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState hair_front_sampler : register(s2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float4> hair_dom : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState hair_dom_sampler : register(s3, space0);
[[vk::binding(4, 0)]] ConstantBuffer<HairVolume> hair_volume : register(b4, space0);
[[vk::binding(5, 0)]] ConstantBuffer<HairMaterial> hair_material : register(b5, space0);

// Tier flags, mirroring render::HairTierCaps.
static const uint kHairCapDualScatter = 1u;
static const uint kHairCapVolume = 2u;
static const uint kHairCapPerFragmentH = 4u;
static const uint kHairCapTilt = 8u;
static const uint kHairCapAuthoredColor = 16u;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 tangent : TANGENT;
  [[vk::location(1)]] float3 world_pos : POSITION1;
  [[vk::location(2)]] float along : TEXCOORD0;
  [[vk::location(3)]] float3 color : COLOR0;
  [[vk::location(4)]] float side : TEXCOORD1;
};

float4 main(PsIn input) : SV_Target0 {
  uint caps = uint(hair_material.caps + 0.5);

  HairSurfaceParams hp;
  hp.beta_m = hair_material.beta_m;
  hp.beta_n = hair_material.beta_n;
  // The groom's per-strand colour is the TARGET colour, inverted to absorption
  // rather than multiplied into the result. That keeps the coupling artists
  // actually depend on: a strand painted blonde does not merely look lighter,
  // it transmits and forward-scatters more, which is what makes it glow.
  // Multiplying the shaded result by the colour instead - the obvious shortcut -
  // gives blonde hair that scatters like brown.
  if ((caps & kHairCapAuthoredColor) != 0u) {
    hp.sigma_a = HairSigmaFromColor(saturate(input.color * pc.tint.rgb),
                                    hair_material.color_reference_depth);
  } else {
    hp.sigma_a = hair_material.sigma_a;
  }
  hp.alpha = hair_material.alpha;
  hp.eta = hair_material.eta;
  hp.density = hair_material.density;
  hp.scatter_scale = hair_material.scatter_scale;

  float3 v = normalize(pc.camera.xyz - input.world_pos);
  float3 l = normalize(-pc.sun.xyz);
  HairFrame frame = HairMakeFrame(input.tangent);
  float3 wo = HairToLocal(frame, v);
  float3 wi = HairToLocal(frame, l);

  // h across the ribbon. The distant tier drops it: at that size the variation
  // is sub-pixel and only shows up as noise.
  float h = (caps & kHairCapPerFragmentH) != 0u ? clamp(input.side, -1.0, 1.0) : 0.0;

  // Fibres between this fragment and the sun. Without the volume the groom has
  // no interior at all - every strand is lit as if it were the only one - so a
  // fallback constant stands in rather than zero, because zero is the one
  // answer that is definitely wrong for hair.
  float fibres = 4.0;
  if ((caps & kHairCapVolume) != 0u && hair_volume.transmittance.enabled > 0.5) {
    fibres = HairFibresToLight(hair_volume.transmittance, hair_dom, hair_dom_sampler,
                               hair_front_depth, hair_front_sampler, input.world_pos);
  }

  if (uint(hair_volume.debug.x + 0.5) == 24u) return float4(HairFibreHeat(fibres), 1.0);

  float3 sun_radiance = pc.sun_color.rgb * pc.sun.w;
  float3 color = HairShade(hp, wo, wi, h, fibres) * sun_radiance;

  // Ambient. Two things have to be right here or the groom desaturates:
  //
  // The albedo is the GROOM's multiple-scattering colour, not one fibre's
  // transmittance. exp(-sigma * D), with the same D the colour inversion uses,
  // recovers it exactly in authored mode and derives the equivalent in pigment
  // mode. Using exp(-sigma * 2) instead - the single-fibre chord - returns
  // nearly white for any realistic absorption, which washes a blonde groom out
  // to olive.
  //
  // And the occlusion decays gently rather than exponentially. A groom's
  // interior is dark, not black: the same multiple scattering that lights it
  // from the sun also carries sky into it, so an exp(-k*n) fill drives the
  // inside to absolute black long before the geometry does.
  float denom = 2.17 + 2.02 * max(hair_material.color_reference_depth, 0.0);
  float3 groom_albedo = exp(-hp.sigma_a * denom);
  float sky_occlusion = 1.0 / (1.0 + 0.15 * fibres);
  color += hair_volume.ambient.rgb * groom_albedo * sky_occlusion;

  // Root darkening: the scalp end of a strand sits inside the groom's densest
  // region, which the volume already knows about at the top of the head but not
  // along a strand that hangs clear of it.
  color *= lerp(0.75, 1.0, input.along);
  return float4(color, 1.0);
}
