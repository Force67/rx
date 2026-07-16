#ifndef RX_GI_SH_HLSLI_
#define RX_GI_SH_HLSLI_

// Minimal 2-band (L0 + L1) spherical harmonics for irradiance. Four coefficients
// per colour channel, packed as one float4 per channel (x = L0, yzw = L1). M1
// only fixes the interface; M2's final gather is the main consumer.
//
// Convention: project a single directional radiance sample with ShProjectAdd,
// accumulate many, then evaluate the cosine-convolved irradiance for a surface
// normal with ShIrradiance. The cosine-lobe convolution weights (A0 = pi,
// A1 = 2*pi/3) are folded into the evaluation, and the 1/pi Lambert factor is
// applied so the result is directly usable as an irradiance term.

struct Sh2 {
  float4 r;  // x: Y0 coeff, yzw: Y1 coeffs (r channel)
  float4 g;
  float4 b;
};

static const float kShY0 = 0.282094792;  // 0.5 * sqrt(1/pi)
static const float kShY1 = 0.488602512;  // 0.5 * sqrt(3/pi)

Sh2 ShZero() {
  Sh2 sh;
  sh.r = 0.0.xxxx;
  sh.g = 0.0.xxxx;
  sh.b = 0.0.xxxx;
  return sh;
}

// Basis values (Y00, Y1-1, Y10, Y11) for a direction. Layout (1, y, z, x) so the
// L1 vector lines up with (dir.y, dir.z, dir.x) for the dot in ShIrradiance.
float4 ShBasis(float3 dir) {
  return float4(kShY0, kShY1 * dir.y, kShY1 * dir.z, kShY1 * dir.x);
}

// Accumulate one radiance sample from direction `dir`, weighted by `weight`
// (e.g. the solid angle of the sample). Call once per gather ray.
void ShProjectAdd(inout Sh2 sh, float3 dir, float3 radiance, float weight) {
  float4 basis = ShBasis(dir) * weight;
  sh.r += basis * radiance.r;
  sh.g += basis * radiance.g;
  sh.b += basis * radiance.b;
}

// Evaluate the raw (non-cosine-convolved) radiance the SH reconstructs along a
// direction. Unlike ShIrradiance this is the incident radiance L(dir), used by
// the specular ray-skip path (AC Shadows: reuse the diffuse SH for rough /
// off-mirror reflection rays instead of tracing). Low-order SH rings, so the
// caller should clamp the result to non-negative.
float3 ShEvaluate(Sh2 sh, float3 dir) {
  float4 basis = ShBasis(dir);
  return float3(dot(sh.r, basis), dot(sh.g, basis), dot(sh.b, basis));
}

// Evaluate the cosine-convolved irradiance for a surface normal. Returns the
// Lambert-ready irradiance (already divided by pi).
float3 ShIrradiance(Sh2 sh, float3 n) {
  // Cosine-lobe zonal weights, with 1/pi Lambert folded in: A0/pi = 1,
  // A1/pi = 2/3.
  const float a0 = 1.0;
  const float a1 = 2.0 / 3.0;
  float4 w = float4(kShY0 * a0, kShY1 * a1 * n.y, kShY1 * a1 * n.z, kShY1 * a1 * n.x);
  return float3(dot(sh.r, w), dot(sh.g, w), dot(sh.b, w));
}

#endif  // RX_GI_SH_HLSLI_
