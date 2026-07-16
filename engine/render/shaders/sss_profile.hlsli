#ifndef RX_SSS_PROFILE_HLSLI_
#define RX_SSS_PROFILE_HLSLI_

// Shared subsurface-scattering math for all three render paths (screen-space
// diffusion, hybrid RT single-scatter, and the pure path tracer). Keeping the
// profile, distance sampling and phase function in one place guarantees skin
// looks the same however it is shaded.
//
// References:
//   Christensen & Burley 2015, "Approximate Reflectance Profiles for Efficient
//     Subsurface Scattering" (the R_d normalized-diffusion profile + CDF).
//   Kulla & Conty 2017, "Revisiting Physically Based Shading at Imageworks"
//     (artist colour/mfp -> single-scattering albedo mapping).
//   Zhang & d'Eon, SIGGRAPH 2025 Advances, "Real-Time SSS via Hybrid
//     ReSTIR-Path-Tracing and Diffusion" (the single-scattering-only residual
//     R1 that turns Burley into a multiple-scattering-only profile so it can be
//     summed with an explicit single-scattering term without double counting).
//
// Length units: sigma_t / sigma_s are per-channel extinction / scattering
// coefficients in 1/world-unit; ell = 1/sigma_t is the mean free path; the
// Burley scale d = ell / s(A). All radii r are in the same world units.

static const float RX_SSS_PI = 3.14159265358979323846;

// --- Kulla-Conty 2017: perceptual colour C -> single-scattering albedo -------
// Inverts the multiple-scattering albedo so an artist authors the diffuse
// colour they want to see. Evaluated per channel. Normally done CPU-side at
// upload; provided here for the LUT/hero path and for reference.
float3 SssAlbedoFromColor(float3 c) {
  return 4.09712 + 4.20863 * c -
         sqrt(9.59217 + 41.6808 * c + 17.7126 * c * c);
}
// s -> single-scattering albedo. g is the phase anisotropy.
float3 SssSingleScatterAlbedo(float3 c, float g) {
  float3 s = SssAlbedoFromColor(c);
  float3 s2 = s * s;
  return (1.0 - s2) / (1.0 - g * s2);
}

// --- Dynamic blood flow (hemoglobin perfusion) -------------------------------
// Couples a hemoglobin concentration `perfusion` (0..1, resting ~0.5) into the
// scattering coefficients and multiple-scatter tint. Oxygenated hemoglobin
// absorbs green/blue far more than red, so more blood reads as redder skin with
// a shorter scatter distance (flush); less blood pales and lightens (blanch).
// sigma_s is unchanged (blood adds absorption, not scattering): sigma_t grows
// as sigma_a = sigma_t - sigma_s grows. Consumed by every render path so the
// blood-flow response is identical however skin is shaded.
static const float3 RX_SSS_HEMO_ABSORB = float3(0.10, 0.55, 0.35);  // rel. per-channel
static const float3 RX_SSS_FLUSH_TINT = float3(0.55, 0.16, 0.14);   // arterial red

void SssApplyPerfusion(inout float3 sigma_t, float3 sigma_s,
                       inout float3 scatter_color, float perfusion) {
  float delta = clamp(perfusion, 0.0, 1.0) - 0.5;  // -0.5 blanch .. +0.5 flush
  float3 sigma_a = max(sigma_t - sigma_s, 0.0);
  // Scale absorption per channel: flush raises green/blue absorption (redder),
  // blanch lowers it (paler). 2x gain spans roughly +/-100% over the range.
  sigma_a *= max(1.0 + 2.0 * delta * RX_SSS_HEMO_ABSORB, 0.0);
  sigma_t = sigma_s + sigma_a;
  // Tint the multiple-scatter colour toward arterial red on flush; desaturate a
  // touch on blanch.
  float flush = saturate(delta);
  float blanch = saturate(-delta);
  scatter_color = saturate(lerp(scatter_color, RX_SSS_FLUSH_TINT, flush * 0.5));
  scatter_color = saturate(lerp(scatter_color, dot(scatter_color, 0.333.xxx).xxx,
                                blanch * 0.35));
}

// Combine a skin material's baseline perfusion with the per-frame blood-flow
// dynamics: a global offset (emotion/exertion), the arterial pulse, and the
// per-vertex tension (stretch/crease blanches). dyn = frame.skin_dynamics
// (x pulse phase rad, y global offset, z pulse amplitude, w tension gain).
float SssEffectivePerfusion(float base, float4 dyn, float tension) {
  float pulse = sin(dyn.x) * dyn.z;
  return saturate(base + dyn.y + pulse - tension * dyn.w);
}

// --- Christensen-Burley normalized diffusion profile -------------------------
// d is the per-channel scale (d = ell / s). r is the radial surface distance.
// R_d(r) = a * (e^{-r/d} + e^{-r/(3d)}) / (8*pi*d*r). The surface albedo a is
// applied by the caller; this returns the geometric profile (a = 1).
float SssBurley(float r, float d) {
  r = max(r, 1e-6);
  d = max(d, 1e-6);
  return (exp(-r / d) + exp(-r / (3.0 * d))) / (8.0 * RX_SSS_PI * d * r);
}

// Radial sampling pdf (already includes the 2*pi*r disk jacobian):
//   p(r) = (e^{-r/d} + e^{-r/(3d)}) / (4d),  integrates to 1 over r in [0, inf).
float SssBurleyPdf(float r, float d) {
  d = max(d, 1e-6);
  return (exp(-r / d) + exp(-r / (3.0 * d))) / (4.0 * d);
}

// CDF (Burley 2015 / Golubev 2019): P(r) = 1 - 0.25 e^{-r/d} - 0.75 e^{-r/(3d)}.
float SssBurleyCdf(float r, float d) {
  d = max(d, 1e-6);
  return 1.0 - 0.25 * exp(-r / d) - 0.75 * exp(-r / (3.0 * d));
}

// Sample a radius from the Burley profile by inverting the CDF. The CDF is
// smooth and monotonic, so a short Newton solve (seeded from the dominant wide
// lobe, clamped to bisection bounds) converges in a handful of steps. Returns a
// radius in the same units as d.
float SssSampleRadius(float d, float u) {
  d = max(d, 1e-6);
  u = clamp(u, 0.0, 0.999999);
  // Effective support: P(r) reaches ~0.999 well within ~16d (the 3d lobe tail).
  float rmax = 16.0 * d;
  if (SssBurleyCdf(rmax, d) < u) return rmax;
  // Seed from the wide (3d) exponential lobe, which carries 0.75 of the mass.
  float r = -3.0 * d * log(1.0 - u);
  r = clamp(r, 1e-6, rmax);
  float lo = 0.0, hi = rmax;
  [unroll] for (int i = 0; i < 12; ++i) {
    float f = SssBurleyCdf(r, d) - u;
    if (f > 0.0) hi = r; else lo = r;
    float deriv = SssBurleyPdf(r, d);
    float step = (deriv > 1e-8) ? f / deriv : 0.0;
    float rn = r - step;
    if (!(rn > lo && rn < hi)) rn = 0.5 * (lo + hi);  // fall back to bisection
    r = rn;
  }
  return r;
}

// --- Single-scattering residual R1 (Zhang & d'Eon 2025, slide 77) ------------
// The first-order Taylor coefficient (in single-scattering albedo) of Burley
// under diffuse transmission, IOR 1.4, semi-infinite flat slab. It is
// ALBEDO-INDEPENDENT, so subtracting it from the full Burley profile yields a
// multiple-scattering-ONLY profile Rn. Use Rn as the diffusion term when an
// explicit single-scattering term (path traced or screen-space marched) is
// added, to avoid double counting. ell = 1/sigma_t is the mean free path.
float SssSingleScatterResidual(float r, float ell) {
  r = max(r, 1e-6);
  ell = max(ell, 1e-6);
  return 0.0423281 * (exp(-5.434 * r / ell) + exp(-1.8113 * r / ell)) /
         (ell * r);
}

// Multiple-scattering-only profile: Burley minus the single-scatter residual,
// clamped to non-negative. d = ell / s is the Burley scale, ell = 1/sigma_t.
float SssMultiScatter(float r, float d, float ell) {
  return max(SssBurley(r, d) - SssSingleScatterResidual(r, ell), 0.0);
}

// --- Henyey-Greenstein phase + importance sampling ---------------------------
float SssHgPhase(float cos_theta, float g) {
  float denom = 1.0 + g * g - 2.0 * g * cos_theta;
  return (1.0 - g * g) / (4.0 * RX_SSS_PI * denom * sqrt(max(denom, 1e-8)));
}
// Returns cos(theta) between the incoming and sampled direction.
float SssSampleHgCos(float g, float u) {
  if (abs(g) < 1e-3) return 1.0 - 2.0 * u;  // isotropic
  float sqr = (1.0 - g * g) / (1.0 - g + 2.0 * g * u);
  return (1.0 + g * g - sqr * sqr) / (2.0 * g);
}
// Orthonormal tangent frame around a normal (for BSSRDF disk sampling).
void SssBuildFrame(float3 n, out float3 t, out float3 b) {
  float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
  t = normalize(cross(up, n));
  b = cross(n, t);
}

// Burley scale d = ell / s(A) per channel, from the mean free path ell = 1/sigma_t
// and the diffuse surface albedo A. s(A) is the Christensen-Burley 2015 fit.
float3 SssBurleyScale(float3 sigma_t, float3 albedo) {
  float3 ell = 1.0 / max(sigma_t, 1e-3);
  float3 A = saturate(albedo);
  float3 s = 1.85 - A + 7.0 * pow(abs(A - 0.8), 3.0);
  return ell / max(s, 1e-3);
}

// Build a world-space direction at angle theta (cos_theta) around `w`.
float3 SssDirectionInFrame(float3 w, float cos_theta, float phi) {
  float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
  float3 up = abs(w.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
  float3 t = normalize(cross(up, w));
  float3 b = cross(w, t);
  return sin_theta * cos(phi) * t + sin_theta * sin(phi) * b + cos_theta * w;
}

// --- Free-flight distance sampling (Beer-Lambert, homogeneous medium) --------
// Unbounded: p(t) = sigma_t e^{-sigma_t t}.
float SssSampleDistance(float sigma_t, float u) {
  return -log(max(1.0 - u, 1e-8)) / max(sigma_t, 1e-6);
}
float SssDistancePdf(float sigma_t, float t) {
  return sigma_t * exp(-sigma_t * t);
}
// Bounded to (0, d]: p(t) = sigma_t e^{-sigma_t t} / (1 - e^{-sigma_t d}). Use
// when the exit distance d is known (less waste / lower variance).
float SssSampleDistanceBounded(float sigma_t, float d, float u) {
  sigma_t = max(sigma_t, 1e-6);
  float span = 1.0 - exp(-sigma_t * d);
  return -log(max(1.0 - u * span, 1e-8)) / sigma_t;
}
float SssDistancePdfBounded(float sigma_t, float d, float t) {
  sigma_t = max(sigma_t, 1e-6);
  float span = 1.0 - exp(-sigma_t * d);
  return sigma_t * exp(-sigma_t * t) / max(span, 1e-8);
}

// --- Spectral (per-channel) distance-sampling MIS (Zhang 2025, slide 104) ----
// Pick a channel by throughput-weighted CDF, then combine the three per-channel
// exponentials with MIS. ss_albedo is the single-scattering albedo (float3).
float SssChannelPdf(float3 sigma_t, float3 ss_albedo, float t) {
  float3 w = sigma_t * ss_albedo;
  float wsum = max(dot(w, 1.0.xxx), 1e-8);
  float3 norm = w / wsum;  // per-channel selection probability
  return dot(norm, sigma_t * exp(-sigma_t * t));
}

#endif  // RX_SSS_PROFILE_HLSLI_
