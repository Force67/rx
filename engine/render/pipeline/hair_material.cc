#include "render/pipeline/hair_material.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rx::render {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kSqrtPiOver8 = 0.626657069f;
constexpr int kPMax = 3;

f32 Sqr(f32 x) { return x * x; }
f32 SafeSqrt(f32 x) { return std::sqrt(std::max(x, 0.0f)); }
f32 SafeAsin(f32 x) { return std::asin(std::clamp(x, -1.0f, 1.0f)); }

f32 I0(f32 x) {
  f32 val = 0.0f;
  f32 x2i = 1.0f;
  f32 ifact = 1.0f;
  int i4 = 1;
  for (int i = 0; i < 10; ++i) {
    if (i > 1) ifact *= static_cast<f32>(i);
    val += x2i / static_cast<f32>(i4) / (ifact * ifact);
    x2i *= x * x;
    i4 *= 4;
  }
  return val;
}

f32 LogI0(f32 x) {
  if (x > 12.0f) {
    return x + 0.5f * (-std::log(2.0f * kPi) + std::log(1.0f / x) + 1.0f / (8.0f * x));
  }
  return std::log(I0(x));
}

f32 Mp(f32 cos_theta_i, f32 cos_theta_o, f32 sin_theta_i, f32 sin_theta_o, f32 v) {
  f32 a = cos_theta_i * cos_theta_o / v;
  f32 b = sin_theta_i * sin_theta_o / v;
  if (v <= 0.1f) {
    return std::exp(LogI0(a) - b - 1.0f / v + 0.6931472f + std::log(1.0f / (2.0f * v)));
  }
  return (std::exp(-b) * I0(a)) / (std::sinh(1.0f / v) * 2.0f * v);
}

f32 FresnelDielectric(f32 cos_theta_i, f32 eta) {
  cos_theta_i = std::clamp(cos_theta_i, -1.0f, 1.0f);
  if (cos_theta_i < 0.0f) {
    eta = 1.0f / eta;
    cos_theta_i = -cos_theta_i;
  }
  f32 sin2_t = (1.0f - cos_theta_i * cos_theta_i) / (eta * eta);
  if (sin2_t >= 1.0f) return 1.0f;
  f32 cos_t = std::sqrt(1.0f - sin2_t);
  f32 rp = (eta * cos_theta_i - cos_t) / (eta * cos_theta_i + cos_t);
  f32 rs = (cos_theta_i - eta * cos_t) / (cos_theta_i + eta * cos_t);
  return 0.5f * (rp * rp + rs * rs);
}

f32 Logistic(f32 x, f32 s) {
  x = std::abs(x);
  f32 e = std::exp(-x / s);
  return e / (s * Sqr(1.0f + e));
}

f32 LogisticCdf(f32 x, f32 s) { return 1.0f / (1.0f + std::exp(-x / s)); }

f32 TrimmedLogistic(f32 x, f32 s) {
  f32 norm = LogisticCdf(kPi, s) - LogisticCdf(-kPi, s);
  return Logistic(x, s) / std::max(norm, 1e-6f);
}

f32 PhiLobe(int p, f32 gamma_o, f32 gamma_t) {
  return 2.0f * static_cast<f32>(p) * gamma_t - 2.0f * gamma_o + static_cast<f32>(p) * kPi;
}

f32 Np(f32 phi, int p, f32 s, f32 gamma_o, f32 gamma_t) {
  f32 dphi = phi - PhiLobe(p, gamma_o, gamma_t);
  dphi = dphi - 2.0f * kPi * std::floor((dphi + kPi) / (2.0f * kPi));
  return TrimmedLogistic(dphi, s);
}

void LobeVariance(f32 beta_m, f32 v[kPMax + 1]) {
  f32 b = std::clamp(beta_m, 0.02f, 1.0f);
  f32 v0 = Sqr(0.726f * b + 0.812f * b * b + 3.7f * std::pow(b, 20.0f));
  v[0] = v0;
  v[1] = 0.25f * v0;
  v[2] = 4.0f * v0;
  v[3] = v[2];
}

f32 AzimuthalScale(f32 beta_n) {
  f32 b = std::clamp(beta_n, 0.02f, 1.0f);
  return kSqrtPiOver8 * (0.265f * b + 1.194f * b * b + 5.372f * std::pow(b, 22.0f));
}

void AverageAttenuation(const HairSurfaceParameters& p, f32 cos_theta_d, f32 a_f[3],
                        f32 a_b[3]) {
  f32 f = FresnelDielectric(std::max(cos_theta_d, 1e-3f), p.eta);
  f32 spread = std::clamp(p.beta_n, 0.0f, 1.0f);
  for (int c = 0; c < 3; ++c) {
    f32 T = std::exp(-p.sigma_a[c] * 2.0f);
    f32 tt = Sqr(1.0f - f) * T;
    f32 trt = tt * T * f;
    a_f[c] = std::clamp(tt * (1.0f - 0.5f * spread) + trt * 0.15f, 0.0f, 1.0f);
    a_b[c] = std::clamp(f * (0.3f + 0.4f * spread) + trt * (0.5f + 0.5f * spread), 0.0f, 1.0f);
  }
}

}  // namespace

void HairSigmaFromMelanin(f32 eumelanin, f32 pheomelanin, f32 out_sigma[3]) {
  const f32 eu[3] = {0.419f, 0.697f, 1.37f};
  const f32 pheo[3] = {0.187f, 0.4f, 1.05f};
  for (int c = 0; c < 3; ++c) {
    out_sigma[c] = std::max(eumelanin, 0.0f) * eu[c] + std::max(pheomelanin, 0.0f) * pheo[c];
  }
}

void HairSigmaFromColorPathTraced(const f32 color[3], f32 beta_n, f32 out_sigma[3]) {
  const f32 b = beta_n;
  const f32 denom = 5.969f - 0.215f * b + 2.532f * b * b - 10.73f * b * b * b +
                    5.574f * b * b * b * b + 0.245f * b * b * b * b * b;
  for (int c = 0; c < 3; ++c) {
    const f32 t = std::log(std::clamp(color[c], 1e-4f, 1.0f)) / std::max(denom, 1e-4f);
    out_sigma[c] = t * t;
  }
}

void HairSigmaFromColor(const f32 color[3], f32 reference_depth, f32 out_sigma[3]) {
  const f32 denom = 2.17f + 2.02f * std::max(reference_depth, 0.0f);
  for (int c = 0; c < 3; ++c) {
    out_sigma[c] = -std::log(std::clamp(color[c], 1e-4f, 1.0f)) / std::max(denom, 1e-3f);
  }
}

HairSurfaceParameters HairPresetParams(HairPreset preset) {
  HairSurfaceParameters p;
  // Melanin concentrations in the ranges reported for human hair. Black hair is
  // almost pure eumelanin at high concentration; red is the only common colour
  // that is pheomelanin-dominant, which is why it is the one that goes orange
  // rather than grey as it lightens.
  switch (preset) {
    case HairPreset::kBlack:
      HairSigmaFromMelanin(8.0f, 0.0f, p.sigma_a);
      p.beta_m = 0.25f;
      p.beta_n = 0.3f;
      break;
    case HairPreset::kBrown:
      HairSigmaFromMelanin(1.3f, 0.0f, p.sigma_a);
      p.beta_m = 0.3f;
      p.beta_n = 0.3f;
      break;
    case HairPreset::kBlonde:
      // Low eumelanin with a little pheomelanin. The forward scattering this
      // produces is the whole point: blonde hair glows because light gets
      // through many fibres, not because it is painted bright.
      HairSigmaFromMelanin(0.2f, 0.35f, p.sigma_a);
      p.beta_m = 0.32f;
      p.beta_n = 0.35f;
      p.scatter_scale = 1.15f;
      break;
    case HairPreset::kRed:
      HairSigmaFromMelanin(0.35f, 2.4f, p.sigma_a);
      p.beta_m = 0.3f;
      p.beta_n = 0.32f;
      break;
    case HairPreset::kGrey:
      HairSigmaFromMelanin(0.35f, 0.0f, p.sigma_a);
      p.beta_m = 0.35f;
      p.beta_n = 0.4f;
      p.scatter_scale = 1.2f;
      break;
    case HairPreset::kWhite:
      // Pigment-free. Almost all of what you see is multiple scattering, so a
      // renderer without it cannot produce white hair at all - it produces grey.
      HairSigmaFromMelanin(0.02f, 0.0f, p.sigma_a);
      p.beta_m = 0.38f;
      p.beta_n = 0.45f;
      p.scatter_scale = 1.3f;
      break;
  }
  return p;
}

void HairResolveSigma(const HairSurfaceParameters& params, const f32 color[3],
                      f32 out_sigma[3]) {
  if (params.color_mode == HairColorMode::kPigment) {
    std::memcpy(out_sigma, params.sigma_a, sizeof(f32) * 3);
    return;
  }
  HairSigmaFromColor(color, params.color_reference_depth, out_sigma);
}

HairTierCaps HairTierApply(HairTier tier, HairSurfaceParameters& params) {
  HairTierCaps caps;
  switch (tier) {
    case HairTier::kHero:
      break;
    case HairTier::kStandard:
      caps.transmittance_volume = false;
      caps.per_fragment_h = false;
      break;
    case HairTier::kDistant:
      caps = {false, false, false, false};
      // At this size a groom is a silhouette. Keeping the lobes alive costs
      // aliasing, not detail, so the fibre is flattened to one broad response.
      params.beta_m = std::max(params.beta_m, 0.5f);
      params.beta_n = std::max(params.beta_n, 0.5f);
      params.alpha = 0.0f;
      params.scatter_scale = 0.0f;
      break;
  }
  return caps;
}

HairRange HairSafeRange(const char* field) {
  auto is = [&](const char* n) { return std::strcmp(field, n) == 0; };
  // Outside these the model stops being physical: roughness below ~0.02 makes
  // the longitudinal lobe narrower than a pixel (pure aliasing), a tilt beyond
  // ~10 degrees separates the highlights further than any real cuticle, and an
  // IOR outside 1.3-1.8 is not keratin.
  if (is("beta_m")) return {0.02f, 1.0f};
  if (is("beta_n")) return {0.02f, 1.0f};
  if (is("alpha")) return {0.0f, 0.175f};  // radians, 0-10 degrees
  if (is("eta")) return {1.3f, 1.8f};
  if (is("density")) return {0.0f, 4.0f};
  if (is("scatter_scale")) return {0.0f, 3.0f};
  if (is("eumelanin")) return {0.0f, 8.0f};
  if (is("pheomelanin")) return {0.0f, 4.0f};
  return {0.0f, 1.0f};
}

void HairEvaluateCpu(const HairSurfaceParameters& p, const f32 wo[3], const f32 wi[3], f32 h,
                     f32 out_rgb[3]) {
  out_rgb[0] = out_rgb[1] = out_rgb[2] = 0.0f;

  const f32 sin_theta_o = std::clamp(wo[0], -1.0f, 1.0f);
  const f32 cos_theta_o = SafeSqrt(1.0f - sin_theta_o * sin_theta_o);
  const f32 phi_o = std::atan2(wo[2], wo[1]);

  const f32 sin_theta_i = std::clamp(wi[0], -1.0f, 1.0f);
  const f32 cos_theta_i = SafeSqrt(1.0f - sin_theta_i * sin_theta_i);
  const f32 phi_i = std::atan2(wi[2], wi[1]);

  const f32 sin_theta_t = sin_theta_o / p.eta;
  const f32 cos_theta_t = SafeSqrt(1.0f - sin_theta_t * sin_theta_t);
  const f32 eta_p =
      SafeSqrt(p.eta * p.eta - sin_theta_o * sin_theta_o) / std::max(cos_theta_o, 1e-5f);
  const f32 sin_gamma_t = std::clamp(h / std::max(eta_p, 1e-5f), -1.0f, 1.0f);
  const f32 cos_gamma_t = SafeSqrt(1.0f - sin_gamma_t * sin_gamma_t);
  const f32 gamma_t = SafeAsin(sin_gamma_t);
  const f32 gamma_o = SafeAsin(std::clamp(h, -1.0f, 1.0f));

  f32 T[3];
  for (int c = 0; c < 3; ++c) {
    T[c] = std::exp(-p.sigma_a[c] * (2.0f * cos_gamma_t / std::max(cos_theta_t, 1e-5f)));
  }

  const f32 cos_gamma_o = SafeSqrt(1.0f - h * h);
  const f32 f = FresnelDielectric(cos_theta_o * cos_gamma_o, p.eta);
  f32 ap[kPMax + 1][3];
  for (int c = 0; c < 3; ++c) {
    ap[0][c] = f;
    ap[1][c] = Sqr(1.0f - f) * T[c];
    ap[2][c] = ap[1][c] * T[c] * f;
    ap[3][c] = ap[2][c] * f * T[c] / std::max(1.0f - T[c] * f, 1e-5f);
  }

  f32 v[kPMax + 1];
  LobeVariance(p.beta_m, v);
  const f32 s = AzimuthalScale(p.beta_n);

  const f32 sin_a = std::sin(p.alpha);
  const f32 cos_a = SafeSqrt(1.0f - sin_a * sin_a);
  const f32 sin2a = 2.0f * sin_a * cos_a;
  const f32 cos2a = cos_a * cos_a - sin_a * sin_a;
  const f32 sin4a = 2.0f * sin2a * cos2a;
  const f32 cos4a = cos2a * cos2a - sin2a * sin2a;

  const f32 phi = phi_i - phi_o;
  for (int lobe = 0; lobe < kPMax; ++lobe) {
    f32 sto, cto;
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
    cto = std::abs(cto);
    const f32 m = Mp(cos_theta_i, cto, sin_theta_i, sto, v[lobe]);
    const f32 n = Np(phi, lobe, s, gamma_o, gamma_t);
    for (int c = 0; c < 3; ++c) out_rgb[c] += m * ap[lobe][c] * n;
  }
  const f32 m_res = Mp(cos_theta_i, cos_theta_o, sin_theta_i, sin_theta_o, v[kPMax]);
  for (int c = 0; c < 3; ++c) {
    out_rgb[c] += m_res * ap[kPMax][c] / (2.0f * kPi);
    // The lobe products are f * cos(theta_i); divide the cosine back out so
    // this returns a BSDF (see the shader's HairEvaluate).
    if (cos_theta_i > 1e-4f) out_rgb[c] /= cos_theta_i;
    out_rgb[c] = std::max(out_rgb[c], 0.0f);
  }
}

void HairShadeCpu(const HairSurfaceParameters& p, const f32 wo[3], const f32 wi[3], f32 h,
                  f32 strand_count, f32 out_rgb[3]) {
  const f32 cos_theta_i = SafeSqrt(1.0f - wi[0] * wi[0]);
  const f32 cos_theta_d =
      std::cos(0.5f * (SafeAsin(wi[0]) - SafeAsin(wo[0])));

  f32 a_f[3], a_b[3];
  AverageAttenuation(p, std::abs(cos_theta_d), a_f, a_b);
  const f32 n = std::max(strand_count, 0.0f) * std::max(p.density, 0.0f);
  const f32 spread = std::sqrt(n) * 0.4f * (0.2f + std::clamp(p.beta_m, 0.0f, 1.0f));

  f32 direct[3];
  HairEvaluateCpu(p, wo, wi, h, direct);

  HairSurfaceParameters blunt = p;
  blunt.beta_m = std::clamp(p.beta_m + spread, 0.0f, 1.0f);
  blunt.beta_n = std::clamp(p.beta_n + spread * 0.5f, 0.0f, 1.0f);
  f32 scattered[3];
  HairEvaluateCpu(blunt, wo, wi, 0.0f, scattered);

  const f32 density_term = 1.0f - std::exp(-0.35f * n);
  for (int c = 0; c < 3; ++c) {
    const f32 forward = std::exp(std::log(std::max(a_f[c], 1e-6f)) * n);
    const f32 back = a_b[c] * density_term * std::max(p.scatter_scale, 0.0f);
    out_rgb[c] = (direct[c] * forward + scattered[c] * forward * back) * cos_theta_i;
  }
}

f32 HairAlbedoCpu(const HairSurfaceParameters& p, const f32 wo[3], u32 theta_steps,
                  u32 phi_steps, int channel) {
  // Integrate f * cos(theta_i) over the full sphere of outgoing directions. For
  // a fibre the "cosine" is cos(theta_i) and the measure is the sphere, not the
  // hemisphere: light leaves a cylinder on every side.
  double total = 0.0;
  const double dtheta = kPi / static_cast<double>(theta_steps);
  const double dphi = 2.0 * kPi / static_cast<double>(phi_steps);
  for (u32 ti = 0; ti < theta_steps; ++ti) {
    const double theta = -0.5 * kPi + (static_cast<double>(ti) + 0.5) * dtheta;
    const double sin_theta = std::sin(theta);
    const double cos_theta = std::cos(theta);
    for (u32 pi_i = 0; pi_i < phi_steps; ++pi_i) {
      const double phi = (static_cast<double>(pi_i) + 0.5) * dphi;
      const f32 wi[3] = {static_cast<f32>(sin_theta),
                         static_cast<f32>(cos_theta * std::cos(phi)),
                         static_cast<f32>(cos_theta * std::sin(phi))};
      f32 rgb[3];
      HairEvaluateCpu(p, wo, wi, 0.0f, rgb);
      total += static_cast<double>(rgb[channel]) * cos_theta * cos_theta * dtheta * dphi;
    }
  }
  return static_cast<f32>(total);
}

}  // namespace rx::render
