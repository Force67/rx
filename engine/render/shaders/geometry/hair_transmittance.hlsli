#ifndef RX_HAIR_TRANSMITTANCE_HLSLI_
#define RX_HAIR_TRANSMITTANCE_HLSLI_

// How many hair fibres sit between a point and the light.
//
// This is the input hair rendering cannot fake. Self-shadowing needs it, dual
// scattering is a function of it, and the shadow a groom casts on the forehead
// under it is the same quantity. A binary shadow map cannot supply it: hair is
// not opaque, and "in shadow / not in shadow" turns a groom into a black cutout
// with a hard edge.
//
// Deep opacity map (Yuksel & Keyser 2008). Pass one records the depth of the
// FRONT-most fibre per light-space texel. Pass two additively accumulates fibre
// counts into four layers spanning a fixed depth past that front surface. The
// layers are anchored to the front rather than to a global slab because that is
// where the density gradient is steepest - the difference between one fibre and
// four is the difference between a lit rim and a shadowed one, and a global
// slab spends all its resolution on the empty air in front of the groom.
//
// The stored quantity is a COUNT, not an opacity, because dual scattering is
// parameterized on the number of fibres crossed rather than on how much light
// they blocked.

// Layer boundaries as a fraction of the layer depth. Packed toward the front:
// the first fibre matters more than the fortieth.
static const float4 RX_HAIR_DOM_LAYERS = float4(0.15, 0.35, 0.65, 1.0);

struct HairTransmittanceParams {
  column_major float4x4 light_view_proj;
  float depth_range;    // metres across the light frustum's near..far
  float layer_depth;    // metres the four layers span past the front fibre
  float fibre_scale;    // rendered ribbons -> optical fibres
  float enabled;        // 0 = the volume did not run this frame
};

// What the hair passes actually bind: the volume plus the frame's ambient, in
// one buffer so a groom's draw touches a single uniform.
struct HairVolume {
  HairTransmittanceParams transmittance;
  float4 ambient;   // rgb sky reaching the groom, w sun removed per crossed fibre
  float4 debug;     // x render::DebugView, yzw unused
};

// Shared heat ramp for the fibre-count debug view, so the strand pass and the
// forward pass colour the same number the same way.
float3 HairFibreHeat(float f) {
  float t = saturate(f / 16.0);
  return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0),
                         1.5 - abs(4.0 * t - 1.0)));
}

// Light-space depth in metres from the frustum's near plane.
float HairDomDepthMetres(float ndc_z, float depth_range) { return ndc_z * depth_range; }

// Fibres in front of `u` (depth past the front fibre, in layer-depth units),
// given a texel's four accumulated layers. Piecewise linear between the layer
// boundaries, which is the right reconstruction for a monotonically increasing
// count.
float HairDomCount(float4 layers, float u) {
  if (u <= RX_HAIR_DOM_LAYERS.x) {
    return layers.x * saturate(u / max(RX_HAIR_DOM_LAYERS.x, 1e-5));
  }
  if (u <= RX_HAIR_DOM_LAYERS.y) {
    float t = (u - RX_HAIR_DOM_LAYERS.x) /
              max(RX_HAIR_DOM_LAYERS.y - RX_HAIR_DOM_LAYERS.x, 1e-5);
    return lerp(layers.x, layers.y, t);
  }
  if (u <= RX_HAIR_DOM_LAYERS.z) {
    float t = (u - RX_HAIR_DOM_LAYERS.y) /
              max(RX_HAIR_DOM_LAYERS.z - RX_HAIR_DOM_LAYERS.y, 1e-5);
    return lerp(layers.y, layers.z, t);
  }
  float t = saturate((u - RX_HAIR_DOM_LAYERS.z) /
                     max(RX_HAIR_DOM_LAYERS.w - RX_HAIR_DOM_LAYERS.z, 1e-5));
  return lerp(layers.z, layers.w, t);
}

// Fibres between `world_pos` and the light. Returns 0 outside the volume (a
// point the groom's light frustum never covered is not shadowed by hair), which
// is also what makes an empty scene cost nothing.
float HairFibresToLight(HairTransmittanceParams p, Texture2D<float4> dom, SamplerState smp,
                        Texture2D<float> front_depth, SamplerState depth_smp, float3 world_pos) {
  float4 clip = mul(p.light_view_proj, float4(world_pos, 1.0));
  if (clip.w <= 0.0) return 0.0;
  float3 ndc = clip.xyz / clip.w;
  float2 uv = ndc.xy * 0.5 + 0.5;
  if (any(uv < 0.0) || any(uv > 1.0) || ndc.z < 0.0 || ndc.z > 1.0) return 0.0;

  float d0 = HairDomDepthMetres(front_depth.SampleLevel(depth_smp, uv, 0.0), p.depth_range);
  float d = HairDomDepthMetres(ndc.z, p.depth_range);
  // In front of every fibre: nothing between this point and the light.
  if (d <= d0) return 0.0;
  float u = (d - d0) / max(p.layer_depth, 1e-4);
  float4 layers = dom.SampleLevel(smp, uv, 0.0);
  return max(HairDomCount(layers, u) * p.fibre_scale, 0.0);
}

#endif  // RX_HAIR_TRANSMITTANCE_HLSLI_
