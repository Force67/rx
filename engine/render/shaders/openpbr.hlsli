#ifndef RX_OPENPBR_HLSLI_
#define RX_OPENPBR_HLSLI_

// Lobe math from the OpenPBR Surface specification v1.1.1
// (AcademySoftwareFoundation/OpenPBR), shared by mesh.ps.hlsl and
// mesh_rt.ps.hlsl. Self-contained on purpose: it is included before those
// shaders declare their own kPi, so it carries its own constant.
//
// This is the "reduction to a mixture of lobes" form the spec sanctions, not
// the full slab model. What is here: energy-preserving Oren-Nayar diffuse,
// F82-tint conductor Fresnel, and the coat absorption / darkening / roughening
// approximations. What is not: Zeltner fuzz, translucent-base volumetrics,
// dispersion, thin-walled mode. See docs/OPENPBR.md.

static const float kRxPi = 3.14159265359;

// --- diffuse -------------------------------------------------------------
// OpenPBR specifies EON: the Fujii form of Oren-Nayar plus an analytic,
// reciprocal energy-compensation term (Portsmouth 2024). Plain Oren-Nayar
// loses energy; EON passes the white furnace test as rho -> 1.
static const float kRxFujiiC1 = 0.5 - 2.0 / (3.0 * kRxPi);
static const float kRxFujiiC2 = 2.0 / 3.0 - 28.0 / (15.0 * kRxPi);

// Directional albedo of the Fujii Oren-Nayar term at angle cosine mu.
float RxEonDirAlbedo(float mu, float roughness) {
  float a = 1.0 / (1.0 + kRxFujiiC1 * roughness);
  float b = roughness * a;
  float si = sqrt(saturate(1.0 - mu * mu));
  float g = si * (acos(clamp(mu, -1.0, 1.0)) - si * mu) +
            2.0 * ((si / max(mu, 1e-4)) * (1.0 - si * si * si) - si) / 3.0;
  return a + b * g / kRxPi;
}

// Hemispherical average of the above, needed by the compensation term.
float RxEonAvgAlbedo(float roughness) {
  float a = 1.0 / (1.0 + kRxFujiiC1 * roughness);
  return a * (1.0 + kRxFujiiC2 * roughness);
}

// The full EON BRDF, already divided by pi so it drops straight in where a
// Lambert `color / kPi` was. At roughness 0 it reduces to exactly that.
float3 RxEonDiffuse(float ndv, float ndl, float ldv, float roughness, float3 color) {
  float s = ldv - ndl * ndv;
  float stinv = s > 0.0 ? s / max(max(ndl, ndv), 1e-4) : s;
  float a = 1.0 / (1.0 + kRxFujiiC1 * roughness);
  float3 single_scatter = color * a * (1.0 + roughness * stinv);

  float e_view = RxEonDirAlbedo(ndv, roughness);
  float e_light = RxEonDirAlbedo(ndl, roughness);
  float e_avg = RxEonAvgAlbedo(roughness);
  float3 ms_color = color * color * e_avg /
                    max(1.0 - color * max(0.0, 1.0 - e_avg), 1e-4);
  float3 multi_scatter = ms_color * max(1e-4, 1.0 - e_view) *
                         max(1e-4, 1.0 - e_light) / max(1e-4, 1.0 - e_avg);
  return (single_scatter + multi_scatter) / kRxPi;
}

// --- metal ---------------------------------------------------------------
// F82-tint conductor Fresnel (Kutz 2021): Schlick, corrected so the curve hits
// a chosen reflectivity at the ~82 degree grazing angle where real metals dip.
// `edge_tint` is OpenPBR's specular_color, expressed as a fraction of the
// Schlick value there, so white reduces this to plain Schlick exactly.
float3 RxFresnelF82(float3 f0, float3 edge_tint, float mu) {
  float3 schlick = f0 + (1.0 - f0) * pow(1.0 - mu, 5.0);
  const float kMuBar = 1.0 / 7.0;
  const float kDenom = kMuBar * pow(1.0 - kMuBar, 6.0);
  float3 schlick_bar = f0 + (1.0 - f0) * pow(1.0 - kMuBar, 5.0);
  // F_schlick(mu_bar) - F(mu_bar), i.e. how far the tint pulls the edge down.
  float3 drop = schlick_bar * (1.0 - edge_tint);
  return schlick - (mu * pow(1.0 - mu, 6.0) / kDenom) * drop;
}

// --- coat ----------------------------------------------------------------
float RxIorToF0(float ior) {
  float f = (ior - 1.0) / (ior + 1.0);
  return f * f;
}

// Hemispherical average of the dielectric Fresnel factor; the standard
// rational fit, used to estimate how much light the coat traps internally.
float RxAvgFresnelDielectric(float ior) {
  return (ior - 1.0) / (4.08567 + 1.00071 * ior);
}

// Absorption in the coat medium along the refracted path, entering and
// leaving. OpenPBR defines coat_color as the SQUARE of the normal-incidence
// transmittance, so at normal incidence this returns coat_color exactly; at
// grazing angles the longer path darkens and saturates it.
float3 RxCoatTransmittance(float3 coat_color, float ndv, float ndl, float ior) {
  float eta2 = max(ior * ior, 1e-4);
  float mu_view = sqrt(saturate(1.0 - (1.0 - ndv * ndv) / eta2));
  float mu_light = sqrt(saturate(1.0 - (1.0 - ndl * ndl) / eta2));
  float path = 1.0 / max(mu_view, 1e-3) + 1.0 / max(mu_light, 1e-3);
  return pow(max(sqrt(max(coat_color, 0.0)), 1e-5), path);
}

// Light that leaves the base and is reflected back into it by the underside of
// the coat gets absorbed again, darkening and saturating the base. K is the
// fraction that returns: near F(ndv) for a mirror-like base, and the larger
// total-internal-reflection value for a Lambertian one, so it is interpolated
// by an estimate of the base roughness. coat_darkening dials the whole effect
// back to none at 0, which is what artists want when the coated colour is
// supposed to match the authored base colour.
float RxCoatDarkening(float base_albedo, float coat_ior, float coat_weight,
                      float darkening, float base_roughness, float ndv) {
  float f_avg = RxAvgFresnelDielectric(coat_ior);
  float eta2 = max(coat_ior * coat_ior, 1e-4);
  float k_rough = 1.0 - (1.0 - f_avg) / eta2;
  float f0 = RxIorToF0(coat_ior);
  float k_smooth = f0 + (1.0 - f0) * pow(1.0 - ndv, 5.0);
  float k = lerp(k_smooth, k_rough, saturate(base_roughness));
  float delta = (1.0 - k) / max(1.0 - saturate(base_albedo) * k, 1e-4);
  return lerp(1.0, delta, saturate(coat_weight * darkening));
}

// A rough coat blurs the lobes beneath it. Treating each NDF as a slope-space
// Gaussian with variance r^4 and convolving them (counting the coat twice,
// since the reflection crosses it on the way in and out) gives this bump.
float RxCoatRoughenBase(float base_roughness, float coat_roughness, float coat_weight) {
  float b4 = base_roughness * base_roughness * base_roughness * base_roughness;
  float c4 = coat_roughness * coat_roughness * coat_roughness * coat_roughness;
  return lerp(base_roughness, pow(min(1.0, b4 + 2.0 * c4), 0.25), saturate(coat_weight));
}

#endif  // RX_OPENPBR_HLSLI_
