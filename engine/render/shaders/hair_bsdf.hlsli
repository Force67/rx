#ifndef RX_HAIR_BSDF_HLSLI_
#define RX_HAIR_BSDF_HLSLI_

// A production hair BSDF: Marschner's R / TT / TRT lobes in the practical,
// controllable parameterization of Chiang et al., plus Zinke's dual-scattering
// approximation for the multiple scattering between strands.
//
// Why this rather than a Kajiya-Kay highlight: a strand is a dielectric
// CYLINDER, and what makes hair read as hair is where light goes after it
// enters one. The surface reflection (R) is only the white sheen. The
// transmitted lobe (TT) is what makes backlit hair glow. The
// transmit-reflect-transmit lobe (TRT) is the coloured secondary highlight and
// the glint. And in anything lighter than black, MOST of the light a viewer
// sees has bounced between many strands - which is why a blonde groom shaded
// with a single-scattering model comes out looking like dark straw no matter
// what colour you paint it.
//
// References:
//   Marschner et al. 2003, "Light Scattering from Human Hair Fibers" (the
//     R/TT/TRT decomposition, the longitudinal/azimuthal separation).
//   d'Eon et al. 2011 (the energy-conserving longitudinal M_p used here).
//   Chiang et al. 2016, "A Practical and Controllable Hair and Fur Model for
//     Production Path Tracing" (the beta_m / beta_n roughness parameterization,
//     the logistic azimuthal N_p, the melanin and colour-inversion mappings).
//   Zinke et al. 2008, "Dual Scattering Approximation for Fast Multiple
//     Scattering in Hair" (the global/local multiple-scattering split).
//
// Frame convention: the strand's local frame has +X along the tangent (root to
// tip). A direction's x component is sin(theta), the longitudinal angle; its
// (y, z) give the azimuth phi. Callers build the frame with HairFrame.
//
// Reciprocity: this model is NOT reciprocal, and that is a property of the
// published formulation rather than of this implementation. The per-lobe
// attenuations and the internal refraction geometry are derived from the
// OUTGOING direction alone, so f(wo -> wi) and f(wi -> wo) differ - measurably,
// by tens of percent on the transmission lobes. It is accepted in production
// because the error sits on lobes that have already been attenuated and because
// the alternative is an order of magnitude more expensive. It does mean this
// BSDF must not be dropped into a bidirectional integrator that assumes
// reciprocity; hair_bsdf_test pins the magnitude so the asymmetry cannot grow
// unnoticed.
//
// `h` is the offset of the shading point across the fibre's width, in [-1, 1].
// A path tracer gets it from the curve intersection; the raster path gets it
// for free from the ribbon expansion, which already knows which side of the
// strand a fragment is on. Faking it (h = 0 everywhere) collapses the azimuthal
// variation and is what makes raster hair look like flat tape.

#ifndef RX_HAIR_PI
#define RX_HAIR_PI 3.14159265358979323846
#endif
#define RX_HAIR_SQRT_PI_OVER_8 0.626657069

// Lobes evaluated explicitly; everything beyond is folded into one isotropic
// residual so no energy is silently dropped.
#define RX_HAIR_PMAX 3

struct HairSurfaceParams {
  float3 sigma_a;        // absorption per unit length, in the fibre's own units
  float beta_m;          // longitudinal roughness, 0..1
  float beta_n;          // azimuthal roughness, 0..1
  float alpha;           // cuticle scale tilt, radians (~2 degrees on human hair)
  float eta;             // index of refraction (1.55 for keratin)
  // Dual scattering controls. `density` scales how much neighbouring-strand
  // scattering a groom gets for a given traversed-strand count; a sparse groom
  // and a dense one with the same fibres are not the same material.
  float density;
  float scatter_scale;   // artist gain on the multiple-scattering term, 1 = as fitted
};

// Keratin, human hair. These are the values to start from, not to ship blindly.
HairSurfaceParams HairDefaultParams() {
  HairSurfaceParams p;
  p.sigma_a = float3(0.06, 0.10, 0.20);
  p.beta_m = 0.3;
  p.beta_n = 0.3;
  p.alpha = 0.0349066;  // 2 degrees
  p.eta = 1.55;
  p.density = 1.0;
  p.scatter_scale = 1.0;
  return p;
}

// --- pigment ----------------------------------------------------------------
// Hair colour is two pigments, not an RGB swatch: eumelanin (brown/black) and
// pheomelanin (red/yellow). Authoring in pigment rather than in albedo is what
// keeps a groom's colour physically coupled to how it scatters - paint a fibre
// "blonde" by lowering absorption and it also gets the forward glow blonde hair
// actually has, which an albedo tint cannot give you.
// Chiang et al. 2016, section 4.
float3 HairSigmaFromMelanin(float eumelanin, float pheomelanin) {
  return max(eumelanin, 0.0) * float3(0.419, 0.697, 1.37) +
         max(pheomelanin, 0.0) * float3(0.187, 0.4, 1.05);
}

// Chiang's published colour inversion, for reference and for a path tracer.
// It is calibrated against FULL multiple scattering - dozens of intra-fibre
// bounces - and it does not apply to this renderer: measured against the dual
// scattering below, asking it for 0.45 renders as 0.77 and asking for 0.60
// renders as 0.90. Kept because it is the right answer for a path-traced groom
// and because the discrepancy is the point.
float3 HairSigmaFromColorPathTraced(float3 color, float beta_n) {
  float b = beta_n;
  float denom = 5.969 - 0.215 * b + 2.532 * b * b - 10.73 * b * b * b +
                5.574 * b * b * b * b + 0.245 * b * b * b * b * b;
  float3 ln_c = log(clamp(color, 1e-4, 1.0));
  float3 t = ln_c / max(denom, 1e-4);
  return t * t;
}

// The inversion THIS renderer uses, fitted against its own transport rather
// than copied from a paper whose transport differs.
//
// Measured law: with dual scattering, the shaded albedo is log-linear in the
// absorption, so sigma = -ln(c) / D with D = 2.17 + 2.02 * n, where n is the
// fibre depth the colour is calibrated at. D turned out to be independent of
// the azimuthal roughness (8.23 to 8.29 across beta_n 0.15..0.75 at n = 3),
// which is why this form is so much simpler than the published one. Round-trip
// error over c in [0.07, 0.85] and n in [3, 10] is under 0.002.
//
// `reference_depth` is where the authored colour is EXACT. Shallower parts of a
// groom read lighter and deeper parts darker, which is what hair does; the
// parameter chooses which part of the groom is the one you painted.
float3 HairSigmaFromColor(float3 color, float reference_depth) {
  float denom = 2.17 + 2.02 * max(reference_depth, 0.0);
  return -log(clamp(color, 1e-4, 1.0)) / max(denom, 1e-3);
}

// --- numerics ---------------------------------------------------------------
float HairSafeSqrt(float x) { return sqrt(max(x, 0.0)); }
float HairSafeAsin(float x) { return asin(clamp(x, -1.0, 1.0)); }
float HairSqr(float x) { return x * x; }

// Modified Bessel function of the first kind, order 0. The series converges
// fast for the arguments M_p produces; the caller switches to the logarithmic
// form before it stops being accurate.
float HairI0(float x) {
  float val = 0.0;
  float x2i = 1.0;
  float ifact = 1.0;
  int i4 = 1;
  [unroll]
  for (int i = 0; i < 10; ++i) {
    if (i > 1) ifact *= float(i);
    val += x2i / float(i4) / (ifact * ifact);
    x2i *= x * x;
    i4 *= 4;
  }
  return val;
}

float HairLogI0(float x) {
  if (x > 12.0) {
    // Asymptotic expansion; the series form overflows well before this.
    return x + 0.5 * (-log(2.0 * RX_HAIR_PI) + log(1.0 / x) + 1.0 / (8.0 * x));
  }
  return log(HairI0(x));
}

// Longitudinal scattering. d'Eon's energy-conserving form: a normalized
// Gaussian-like detector on the sphere rather than a Gaussian in the angle,
// which is what stops the lobes losing energy as they narrow.
float HairMp(float cos_theta_i, float cos_theta_o, float sin_theta_i, float sin_theta_o,
             float v) {
  float a = cos_theta_i * cos_theta_o / v;
  float b = sin_theta_i * sin_theta_o / v;
  if (v <= 0.1) {
    // Log-space below v = 0.1: sinh(1/v) overflows and the ratio loses every
    // significant digit long before the lobe stops being useful.
    return exp(HairLogI0(a) - b - 1.0 / v + 0.6931472 + log(1.0 / (2.0 * v)));
  }
  return (exp(-b) * HairI0(a)) / (sinh(1.0 / v) * 2.0 * v);
}

// Dielectric Fresnel for unpolarized light.
float HairFresnel(float cos_theta_i, float eta) {
  cos_theta_i = clamp(cos_theta_i, -1.0, 1.0);
  if (cos_theta_i < 0.0) {
    eta = 1.0 / eta;
    cos_theta_i = -cos_theta_i;
  }
  float sin2_t = (1.0 - cos_theta_i * cos_theta_i) / (eta * eta);
  if (sin2_t >= 1.0) return 1.0;  // total internal reflection
  float cos_t = sqrt(1.0 - sin2_t);
  float rp = (eta * cos_theta_i - cos_t) / (eta * cos_theta_i + cos_t);
  float rs = (cos_theta_i - eta * cos_t) / (cos_theta_i + eta * cos_t);
  return 0.5 * (rp * rp + rs * rs);
}

// Per-lobe attenuation: how much energy survives to leave through each path.
// A[0] = R (surface reflection), A[1] = TT (straight through), A[2] = TRT (the
// coloured secondary), A[3] = everything else, summed as a geometric series so
// the tail is accounted for rather than clipped.
void HairAp(float cos_theta_o, float eta, float h, float3 T, out float3 ap[RX_HAIR_PMAX + 1]) {
  float cos_gamma_o = HairSafeSqrt(1.0 - h * h);
  float cos_theta = cos_theta_o * cos_gamma_o;
  float f = HairFresnel(cos_theta, eta);
  ap[0] = f.xxx;
  ap[1] = HairSqr(1.0 - f) * T;
  ap[2] = ap[1] * T * f;
  ap[3] = ap[2] * f * T / max(1.0 - T * f, 1e-5);
}

float HairLogistic(float x, float s) {
  x = abs(x);
  float e = exp(-x / s);
  return e / (s * HairSqr(1.0 + e));
}

float HairLogisticCdf(float x, float s) { return 1.0 / (1.0 + exp(-x / s)); }

// Logistic trimmed to [-pi, pi] and renormalized. Chiang's substitute for the
// Gaussian: it has a closed-form CDF, so the same distribution can be sampled
// exactly in a path tracer and evaluated here, and the two cannot disagree.
float HairTrimmedLogistic(float x, float s) {
  float norm = HairLogisticCdf(RX_HAIR_PI, s) - HairLogisticCdf(-RX_HAIR_PI, s);
  return HairLogistic(x, s) / max(norm, 1e-6);
}

float HairPhi(int p, float gamma_o, float gamma_t) {
  return 2.0 * float(p) * gamma_t - 2.0 * gamma_o + float(p) * RX_HAIR_PI;
}

float HairNp(float phi, int p, float s, float gamma_o, float gamma_t) {
  float dphi = phi - HairPhi(p, gamma_o, gamma_t);
  // Wrap into [-pi, pi]. A loop would be unbounded for the large offsets p = 2
  // produces, so fold it arithmetically.
  dphi = dphi - 2.0 * RX_HAIR_PI * floor((dphi + RX_HAIR_PI) / (2.0 * RX_HAIR_PI));
  return HairTrimmedLogistic(dphi, s);
}

// Longitudinal variances per lobe. The TT lobe is tighter and the TRT lobe
// broader than R by the fixed ratios Marschner measured.
void HairLobeVariance(float beta_m, out float v[RX_HAIR_PMAX + 1]) {
  float b = clamp(beta_m, 0.02, 1.0);
  float v0 = HairSqr(0.726 * b + 0.812 * b * b + 3.7 * pow(b, 20.0));
  v[0] = v0;
  v[1] = 0.25 * v0;
  v[2] = 4.0 * v0;
  v[3] = v[2];
}

float HairAzimuthalScale(float beta_n) {
  float b = clamp(beta_n, 0.02, 1.0);
  return RX_HAIR_SQRT_PI_OVER_8 *
         (0.265 * b + 1.194 * b * b + 5.372 * pow(b, 22.0));
}

// --- the local frame --------------------------------------------------------
// Builds an orthonormal frame with +X along the strand tangent. The other two
// axes are arbitrary but must be CONSISTENT between the two directions handed
// to HairEvaluate, since only their difference (the azimuth) is used.
struct HairFrame {
  float3 t;  // along the strand
  float3 b1;
  float3 b2;
};

HairFrame HairMakeFrame(float3 tangent) {
  HairFrame f;
  f.t = normalize(tangent);
  float3 up = abs(f.t.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  f.b1 = normalize(cross(up, f.t));
  f.b2 = cross(f.t, f.b1);
  return f;
}

float3 HairToLocal(HairFrame f, float3 w) {
  return float3(dot(w, f.t), dot(w, f.b1), dot(w, f.b2));
}

// --- single scattering ------------------------------------------------------
// wo / wi are in the strand's local frame (HairToLocal). Returns the BSDF
// itself - hair's "cosine" is cos(theta_i) and the caller applies it, because
// the dual-scattering terms below need the bare BSDF. Note the division at the
// end: the lobe products are f * cos(theta_i), so the cosine comes back out
// here rather than being applied twice downstream.
float3 HairEvaluate(HairSurfaceParams p, float3 wo, float3 wi, float h) {
  float sin_theta_o = clamp(wo.x, -1.0, 1.0);
  float cos_theta_o = HairSafeSqrt(1.0 - sin_theta_o * sin_theta_o);
  float phi_o = atan2(wo.z, wo.y);

  float sin_theta_i = clamp(wi.x, -1.0, 1.0);
  float cos_theta_i = HairSafeSqrt(1.0 - sin_theta_i * sin_theta_i);
  float phi_i = atan2(wi.z, wi.y);

  // Refraction inside the fibre. eta_p is the effective IOR for the projected
  // (azimuthal) problem after Bravais' law, which is what lets a 3D cylinder be
  // solved as a 2D circle.
  float sin_theta_t = sin_theta_o / p.eta;
  float cos_theta_t = HairSafeSqrt(1.0 - sin_theta_t * sin_theta_t);
  float eta_p = HairSafeSqrt(p.eta * p.eta - sin_theta_o * sin_theta_o) / max(cos_theta_o, 1e-5);
  float sin_gamma_t = clamp(h / max(eta_p, 1e-5), -1.0, 1.0);
  float cos_gamma_t = HairSafeSqrt(1.0 - sin_gamma_t * sin_gamma_t);
  float gamma_t = HairSafeAsin(sin_gamma_t);
  float gamma_o = HairSafeAsin(clamp(h, -1.0, 1.0));

  // Beer-Lambert along the chord the refracted ray takes through the fibre.
  float3 T = exp(-p.sigma_a * (2.0 * cos_gamma_t / max(cos_theta_t, 1e-5)));

  float3 ap[RX_HAIR_PMAX + 1];
  HairAp(cos_theta_o, p.eta, h, T, ap);

  float v[RX_HAIR_PMAX + 1];
  HairLobeVariance(p.beta_m, v);
  float s = HairAzimuthalScale(p.beta_n);

  // Cuticle scale tilt: the scales on a real fibre are angled, which shifts the
  // R lobe toward the tip and the TRT lobe toward the root. That separation is
  // the reason a real highlight is two offset bands and not one - remove it and
  // hair immediately reads as plastic tubing.
  float sin_a = sin(p.alpha);
  float cos_a = HairSafeSqrt(1.0 - sin_a * sin_a);
  float sin2a = 2.0 * sin_a * cos_a;
  float cos2a = cos_a * cos_a - sin_a * sin_a;
  float sin4a = 2.0 * sin2a * cos2a;
  float cos4a = cos2a * cos2a - sin2a * sin2a;

  float phi = phi_i - phi_o;
  float3 sum = float3(0.0, 0.0, 0.0);

  [unroll]
  for (int lobe = 0; lobe < RX_HAIR_PMAX; ++lobe) {
    float sto, cto;
    if (lobe == 0) {
      sto = sin_theta_o * cos2a - cos_theta_o * sin2a;
      cto = cos_theta_o * cos2a + sin_theta_o * sin2a;
    } else if (lobe == 1) {
      sto = sin_theta_o * cos_a + cos_theta_o * sin_a;
      cto = cos_theta_o * cos_a - sin_theta_o * sin_a;
    } else {
      sto = sin_theta_o * cos4a + cos_theta_o * sin4a;
      cto = cos_theta_o * cos4a - sin_theta_o * sin4a;
    }
    cto = abs(cto);
    sum += HairMp(cos_theta_i, cto, sin_theta_i, sto, v[lobe]) * ap[lobe] *
           HairNp(phi, lobe, s, gamma_o, gamma_t);
  }
  // The residual lobe is isotropic in azimuth; without it every fibre lighter
  // than dark brown loses a visible slice of its energy.
  sum += HairMp(cos_theta_i, cos_theta_o, sin_theta_i, sin_theta_o, v[RX_HAIR_PMAX]) *
         ap[RX_HAIR_PMAX] / (2.0 * RX_HAIR_PI);
  // M_p is normalized against the cylinder's cos(theta_i) measure, so the sum
  // above is f * cos(theta_i). Divide it back out so this returns a BSDF and
  // the caller's cosine is the only one applied.
  if (cos_theta_i > 1e-4) sum /= cos_theta_i;
  return max(sum, 0.0);
}

// --- dual scattering --------------------------------------------------------
// Zinke's observation: in a groom, the light that reaches a fibre has already
// passed through others (FORWARD scattering, which attenuates and spreads it),
// and the light a viewer sees also includes what came back out of the
// neighbours (BACKWARD scattering). Modelling only the first fibre is what
// makes light hair render dark.
//
// The exact formulation integrates the BSDF over the sphere per shading point.
// This is the real-time reduction: the average attenuations are evaluated
// analytically from the fibre's own absorption, and the count of fibres between
// the shading point and the light comes from the hair transmittance volume
// (geometry/hair_transmittance.h). Both approximations are stated where they
// are made, because the failure mode of dual scattering is that it looks
// plausible while being wrong by a constant factor.
struct HairScattering {
  float3 forward;  // T_f, what survives the trip in
  float3 back;     // the neighbours' contribution back toward the eye
  float spread;    // extra longitudinal variance the forward trip added
};

// Average attenuation of a fibre in the forward and backward hemispheres. The
// forward half is dominated by TT (light going on through), the backward half
// by R and TRT (light turning around). Fitted against the single-scattering
// evaluation above rather than taken from the paper's tables, so the two halves
// of this file stay consistent when the BSDF changes.
void HairAverageAttenuation(HairSurfaceParams p, float cos_theta_d, out float3 a_f,
                            out float3 a_b) {
  float3 T = exp(-p.sigma_a * 2.0);  // a chord through the middle of the fibre
  float f = HairFresnel(max(cos_theta_d, 1e-3), p.eta);
  float3 tt = HairSqr(1.0 - f) * T;
  float3 trt = tt * T * f;
  // Roughness widens both halves toward the isotropic average; a very smooth
  // fibre sends almost everything forward.
  float spread = saturate(p.beta_n);
  a_f = saturate(tt * (1.0 - 0.5 * spread) + trt * 0.15);
  a_b = saturate(f.xxx * (0.3 + 0.4 * spread) + trt * (0.5 + 0.5 * spread));
}

// strand_count: fibres between this point and the light, from the transmittance
// volume. 0 means fully exposed.
HairScattering HairDualScattering(HairSurfaceParams p, float cos_theta_d, float strand_count) {
  HairScattering s;
  float3 a_f, a_b;
  HairAverageAttenuation(p, cos_theta_d, a_f, a_b);

  float n = max(strand_count, 0.0) * max(p.density, 0.0);
  // T_f = a_f^n. Evaluated in log space so a groom hundreds of fibres deep does
  // not underflow to a hard black core.
  s.forward = exp(log(max(a_f, 1e-6)) * n);

  // Each crossing widens the incoming lobe. Zinke tracks the accumulated
  // variance; the square-root growth is the random-walk result.
  s.spread = sqrt(n) * 0.4 * (0.2 + saturate(p.beta_m));

  // Local (neighbour) back-scattering. It saturates rather than growing with
  // depth - past a few fibres the extra ones are already dark - and it is what
  // fills the interior of a blonde groom instead of leaving a black cavity.
  float density_term = 1.0 - exp(-0.35 * n);
  s.back = a_b * density_term * max(p.scatter_scale, 0.0);
  return s;
}

// Combines everything for one punctual light. `wo`/`wi` are in the strand
// frame, `strand_count` comes from the transmittance volume, and the returned
// value is already cosine-weighted and ready to multiply by the light radiance.
float3 HairShade(HairSurfaceParams p, float3 wo, float3 wi, float h, float strand_count) {
  float cos_theta_i = HairSafeSqrt(1.0 - wi.x * wi.x);
  // theta_d, the half longitudinal angle, is what the average attenuations are
  // parameterized on.
  float cos_theta_d = cos(0.5 * (asin(clamp(wi.x, -1.0, 1.0)) - asin(clamp(wo.x, -1.0, 1.0))));
  HairScattering s = HairDualScattering(p, abs(cos_theta_d), strand_count);

  float3 direct = HairEvaluate(p, wo, wi, h) * s.forward;

  // The multiply-scattered light arrives spread out, so it is evaluated against
  // a deliberately blunted copy of the fibre rather than the sharp one: a
  // highlight in the multiple-scattering term is a highlight that never
  // survived the walk to get there.
  HairSurfaceParams blunt = p;
  blunt.beta_m = saturate(p.beta_m + s.spread);
  blunt.beta_n = saturate(p.beta_n + s.spread * 0.5);
  float3 scattered = HairEvaluate(blunt, wo, wi, 0.0) * s.forward * s.back;

  return (direct + scattered) * cos_theta_i;
}

#endif  // RX_HAIR_BSDF_HLSLI_
