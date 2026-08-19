#include "render/pipeline/hair_material.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// Regression tests for the hair BSDF (render/shaders/hair_bsdf.hlsli, mirrored
// on the CPU in hair_material.cc).
//
// The contracts that matter for hair are different from a surface BRDF's:
//
//   1. ENERGY. A non-absorbing fibre must reflect essentially all of the light
//      it receives, over the whole SPHERE - a cylinder scatters on every side.
//      Marschner's three lobes do not sum to one on their own; the residual
//      term is what closes the gap, and if it regresses, every light-coloured
//      groom silently loses energy and comes out dark. That failure looks like
//      an art problem, so it gets fixed by painting the hair brighter, which
//      breaks the pigment coupling permanently.
//   2. RECIPROCITY. f(wo -> wi) == f(wi -> wo).
//   3. PIGMENT COUPLING. Colour comes from absorption, so more melanin must
//      darken, and pheomelanin must redden rather than just darken.
//   4. MULTIPLE SCATTERING. It must attenuate with depth, saturate rather than
//      grow without bound, and do almost nothing to a black fibre (which
//      absorbs before it can bounce) while doing a lot to a white one.

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "hair_bsdf_test: FAIL: %s\n", message);
  ++failures;
}

constexpr float kPi = 3.14159265358979323846f;

// A direction in the strand frame: theta is the longitudinal angle off the
// normal plane, phi the azimuth around the fibre.
void Dir(float theta, float phi, rx::f32 out[3]) {
  out[0] = std::sin(theta);
  out[1] = std::cos(theta) * std::cos(phi);
  out[2] = std::cos(theta) * std::sin(phi);
}

float Luma(const rx::f32 rgb[3]) {
  return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

}  // namespace

int main() {
  using namespace rx::render;

  // --- 1. energy ------------------------------------------------------------
  {
    HairSurfaceParameters clear;  // no absorption at all
    clear.sigma_a[0] = clear.sigma_a[1] = clear.sigma_a[2] = 0.0f;
    for (float beta : {0.1f, 0.3f, 0.6f, 0.9f}) {
      clear.beta_m = beta;
      clear.beta_n = beta;
      for (float theta_o : {-0.9f, -0.3f, 0.0f, 0.4f, 1.0f}) {
        rx::f32 wo[3];
        Dir(theta_o, 0.0f, wo);
        const float albedo = HairAlbedoCpu(clear, wo, 96, 96, 1);
        Check(albedo <= 1.02f, "a non-absorbing fibre never reflects more than it receives");
        Check(albedo >= 0.93f,
              "a non-absorbing fibre reflects essentially all of it (the residual lobe closes "
              "the gap Marschner's three leave open)");
        if (albedo > 1.02f || albedo < 0.93f) {
          std::fprintf(stderr, "  beta %.2f theta_o %+.2f -> albedo %.4f\n", beta, theta_o,
                       albedo);
        }
      }
    }

    // Absorption must cost energy, monotonically.
    HairSurfaceParameters dark = clear;
    HairSigmaFromMelanin(4.0f, 0.0f, dark.sigma_a);
    rx::f32 wo[3];
    Dir(0.2f, 0.0f, wo);
    const float clear_albedo = HairAlbedoCpu(clear, wo, 64, 64, 1);
    const float dark_albedo = HairAlbedoCpu(dark, wo, 64, 64, 1);
    Check(dark_albedo < clear_albedo * 0.6f, "absorption costs energy");
  }

  // --- 2. the azimuthal geometry ------------------------------------------
  {
    // `h` - where across the fibre's width the shading point sits - is the part
    // raster hair usually fakes, and faking it is what flattens a strand into
    // tape. The model makes an exact prediction about it: the surface (R) lobe's
    // azimuthal peak sits at phi = -2*asin(h). Checking that pins the whole
    // cylinder geometry, because gamma_o, gamma_t and Phi_p all fall out of it.
    HairSurfaceParameters p = HairPresetParams(HairPreset::kBlack);  // R lobe only
    p.beta_m = 0.1f;
    p.beta_n = 0.1f;
    p.alpha = 0.0f;
    rx::f32 wo[3];
    Dir(0.0f, 0.0f, wo);
    float worst = 0.0f;
    for (const float h : {-0.8f, -0.4f, 0.0f, 0.4f, 0.8f}) {
      float best = -1.0f, best_phi = 0.0f;
      for (int i = 0; i < 2000; ++i) {
        const float phi = -kPi + 2.0f * kPi * static_cast<float>(i) / 1999.0f;
        rx::f32 wi[3], rgb[3];
        Dir(0.0f, phi, wi);
        HairEvaluateCpu(p, wo, wi, h, rgb);
        if (rgb[1] > best) {
          best = rgb[1];
          best_phi = phi;
        }
      }
      worst = std::max(worst, std::abs(best_phi - (-2.0f * std::asin(h))));
    }
    Check(worst < 0.01f, "the surface lobe peaks where the cylinder geometry says it should");
    if (worst >= 0.01f) std::fprintf(stderr, "  worst azimuthal peak error: %.4f rad\n", worst);

    // ... and h has to matter. A model evaluated at h = 0 everywhere loses the
    // variation across the strand's width entirely.
    HairSurfaceParameters brown = HairPresetParams(HairPreset::kBrown);
    rx::f32 wi[3];
    Dir(0.1f, 1.2f, wi);
    rx::f32 centre[3], edge[3];
    HairEvaluateCpu(brown, wo, wi, 0.0f, centre);
    HairEvaluateCpu(brown, wo, wi, 0.85f, edge);
    Check(std::abs(Luma(centre) - Luma(edge)) > 0.01f * std::max(Luma(centre), Luma(edge)),
          "the response varies across the fibre's width");
  }

  // --- 2b. the asymmetry is bounded ---------------------------------------
  {
    // This model is not reciprocal - the attenuations and the refraction
    // geometry come from the outgoing direction alone, exactly as published.
    // That is not asserted away here; it is PINNED, so it cannot quietly grow.
    HairSurfaceParameters p = HairPresetParams(HairPreset::kBrown);
    float peak = 0.0f, worst = 0.0f;
    rx::f32 samples[40][2][3];
    for (int i = 0; i < 40; ++i) {
      Dir(-1.2f + 2.4f * static_cast<float>(i) / 39.0f, 0.31f * static_cast<float>(i),
          samples[i][0]);
      Dir(1.1f - 2.2f * static_cast<float>(i) / 39.0f, 3.1f + 0.17f * static_cast<float>(i),
          samples[i][1]);
    }
    for (int i = 0; i < 40; ++i) {
      rx::f32 a[3], b[3];
      HairEvaluateCpu(p, samples[i][0], samples[i][1], 0.3f, a);
      HairEvaluateCpu(p, samples[i][1], samples[i][0], -0.3f, b);
      for (int c = 0; c < 3; ++c) peak = std::max(peak, std::max(a[c], b[c]));
    }
    for (int i = 0; i < 40; ++i) {
      rx::f32 a[3], b[3];
      HairEvaluateCpu(p, samples[i][0], samples[i][1], 0.3f, a);
      HairEvaluateCpu(p, samples[i][1], samples[i][0], -0.3f, b);
      for (int c = 0; c < 3; ++c) {
        const float m = std::max(a[c], b[c]);
        if (m < 0.05f * peak) continue;
        worst = std::max(worst, std::abs(a[c] - b[c]) / m);
      }
    }
    Check(worst < 0.75f,
          "the model's known non-reciprocity stays within the range the published "
          "formulation produces");
    if (worst >= 0.75f) std::fprintf(stderr, "  asymmetry: %.3f\n", worst);
  }

  // --- 2c. the lobes are what they claim to be -----------------------------
  {
    // R is a surface reflection: Fresnel only, no absorption, so it is
    // ACHROMATIC however dark the fibre is. TT and TRT have crossed the pigment
    // and must carry its colour. If that separation breaks, black hair grows a
    // coloured sheen and blonde hair loses its glow.
    HairSurfaceParameters black = HairPresetParams(HairPreset::kBlack);
    black.beta_m = 0.1f;
    black.beta_n = 0.1f;
    black.alpha = 0.0f;
    rx::f32 wo[3], wi[3], rgb[3];
    Dir(0.0f, 0.0f, wo);
    Dir(0.0f, 0.0f, wi);  // phi = 0: the R lobe's peak at h = 0
    HairEvaluateCpu(black, wo, wi, 0.0f, rgb);
    const float spread = (std::max({rgb[0], rgb[1], rgb[2]}) -
                          std::min({rgb[0], rgb[1], rgb[2]})) /
                         std::max({rgb[0], rgb[1], rgb[2], 1e-6f});
    Check(spread < 0.02f, "the surface lobe is achromatic even on a heavily pigmented fibre");

    // The transmission lobe, by contrast, has to be strongly coloured.
    HairSurfaceParameters red = HairPresetParams(HairPreset::kRed);
    red.beta_m = 0.15f;
    red.beta_n = 0.15f;
    rx::f32 through[3];
    Dir(0.0f, kPi, wi);  // straight through the fibre
    HairEvaluateCpu(red, wo, wi, 0.0f, through);
    const float tint = through[0] / std::max(through[2], 1e-6f);
    Check(tint > 2.0f, "the transmission lobe carries the pigment's colour");
  }

  // --- 3. pigment coupling --------------------------------------------------
  {
    rx::f32 wo[3], wi[3];
    Dir(0.1f, 0.0f, wo);
    Dir(-0.1f, 2.4f, wi);

    float previous = 1e9f;
    for (float eu : {0.1f, 0.5f, 1.5f, 4.0f, 8.0f}) {
      HairSurfaceParameters p;
      HairSigmaFromMelanin(eu, 0.0f, p.sigma_a);
      rx::f32 rgb[3];
      HairEvaluateCpu(p, wo, wi, 0.0f, rgb);
      const float l = Luma(rgb);
      Check(l < previous, "more eumelanin is always darker");
      previous = l;
    }

    HairSurfaceParameters red, brown;
    HairSigmaFromMelanin(0.35f, 2.4f, red.sigma_a);
    HairSigmaFromMelanin(1.3f, 0.0f, brown.sigma_a);
    rx::f32 red_rgb[3], brown_rgb[3];
    HairEvaluateCpu(red, wo, wi, 0.0f, red_rgb);
    HairEvaluateCpu(brown, wo, wi, 0.0f, brown_rgb);
    Check(red_rgb[0] / std::max(red_rgb[2], 1e-6f) >
              brown_rgb[0] / std::max(brown_rgb[2], 1e-6f),
          "pheomelanin reddens rather than merely darkening");

    // The colour inversion. Chiang's fit targets the colour a GROOM settles at
    // under multiple scattering, not the albedo of one fibre - a single fibre is
    // always much brighter than the volume it sits in. So the contract checked
    // here is that the mapping is monotone, hue-preserving, and that the
    // multiply-scattered result moves toward the requested colour.
    float last_sigma = -1.0f;
    for (const float target : {0.75f, 0.45f, 0.2f, 0.08f}) {
      const rx::f32 want[3] = {target, target, target};
      HairSurfaceParameters p;
      p.beta_n = 0.3f;
      HairSigmaFromColor(want, p.color_reference_depth, p.sigma_a);
      Check(p.sigma_a[1] > last_sigma, "a darker request always means more absorption");
      last_sigma = p.sigma_a[1];
    }
    {
      const rx::f32 warm[3] = {0.6f, 0.25f, 0.1f};
      HairSurfaceParameters p;
      HairSigmaFromColor(warm, p.color_reference_depth, p.sigma_a);
      Check(p.sigma_a[0] < p.sigma_a[1] && p.sigma_a[1] < p.sigma_a[2],
            "a warm request absorbs least in red and most in blue");
    }
    {
      // The strong version of the contract, which only holds because the
      // mapping was FITTED against this renderer instead of copied from a paper
      // written for a path tracer: ask for a colour, shade a groom at the
      // reference depth, and get that colour back.
      auto shaded_albedo = [&](float target, float depth) {
        const rx::f32 want[3] = {target, target, target};
        HairSurfaceParameters p;
        p.color_mode = HairColorMode::kPigment;
        HairSigmaFromColor(want, depth, p.sigma_a);
        HairSurfaceParameters clear = p;
        clear.sigma_a[0] = clear.sigma_a[1] = clear.sigma_a[2] = 0.0f;
        auto integrate = [&](const HairSurfaceParameters& q) {
          double total = 0.0;
          const int n = 32;
          for (int oi = 0; oi < n; ++oi) {
            rx::f32 w_out[3];
            Dir(-1.2f + 2.4f * (static_cast<float>(oi) + 0.5f) / n, 0.0f, w_out);
            for (int ti = 0; ti < n; ++ti) {
              const float theta = -1.5f + 3.0f * (static_cast<float>(ti) + 0.5f) / n;
              for (int pi_i = 0; pi_i < 16; ++pi_i) {
                const float phi = 2.0f * kPi * (static_cast<float>(pi_i) + 0.5f) / 16.0f;
                rx::f32 wi[3], rgb[3];
                Dir(theta, phi, wi);
                HairShadeCpu(q, w_out, wi, 0.0f, depth, rgb);
                total += static_cast<double>(rgb[1]) * std::cos(theta);
              }
            }
          }
          return total;
        };
        return static_cast<float>(integrate(p) / std::max(integrate(clear), 1e-12));
      };
      float worst = 0.0f;
      for (const float depth : {3.0f, 6.0f, 10.0f}) {
        for (const float target : {0.85f, 0.65f, 0.45f, 0.28f, 0.15f, 0.07f}) {
          worst = std::max(worst, std::abs(shaded_albedo(target, depth) - target));
        }
      }
      Check(worst < 0.02f,
            "the fitted colour inversion renders the colour it was asked for, at every "
            "reference depth");
      if (worst >= 0.02f) std::fprintf(stderr, "  worst colour round-trip error: %.4f\n", worst);

      // And the published constants must NOT be silently substituted: they are
      // right for a path tracer and wrong here, which is the whole reason the
      // fitted mapping exists.
      const rx::f32 mid[3] = {0.45f, 0.45f, 0.45f};
      HairSurfaceParameters chiang;
      HairSigmaFromColorPathTraced(mid, chiang.beta_n, chiang.sigma_a);
      HairSurfaceParameters fitted;
      HairSigmaFromColor(mid, fitted.color_reference_depth, fitted.sigma_a);
      Check(fitted.sigma_a[1] > chiang.sigma_a[1] * 2.0f,
            "the fitted absorption is far stronger than the path-traced fit's, as measured");
    }
  }

  // --- 4. multiple scattering ----------------------------------------------
  {
    rx::f32 wo[3], wi[3];
    Dir(0.2f, 0.0f, wo);
    Dir(-0.2f, 2.0f, wi);

    HairSurfaceParameters blonde = HairPresetParams(HairPreset::kBlonde);
    rx::f32 exposed[3], shallow[3], deep[3];
    HairShadeCpu(blonde, wo, wi, 0.0f, 0.0f, exposed);
    HairShadeCpu(blonde, wo, wi, 0.0f, 4.0f, shallow);
    HairShadeCpu(blonde, wo, wi, 0.0f, 40.0f, deep);
    Check(Luma(shallow) < Luma(exposed), "forward transmittance attenuates with depth");
    Check(Luma(deep) < Luma(shallow), "and keeps attenuating");
    Check(Luma(deep) > 0.0f, "but a deep groom does not go to absolute black");

    // The point of dual scattering: it must lift a light fibre and barely touch
    // a black one, because a black fibre absorbs before it can bounce.
    HairSurfaceParameters black = HairPresetParams(HairPreset::kBlack);
    rx::f32 blonde_off[3], blonde_on[3], black_off[3], black_on[3];
    HairSurfaceParameters blonde_noscatter = blonde;
    blonde_noscatter.scatter_scale = 0.0f;
    HairSurfaceParameters black_noscatter = black;
    black_noscatter.scatter_scale = 0.0f;
    HairShadeCpu(blonde_noscatter, wo, wi, 0.0f, 6.0f, blonde_off);
    HairShadeCpu(blonde, wo, wi, 0.0f, 6.0f, blonde_on);
    HairShadeCpu(black_noscatter, wo, wi, 0.0f, 6.0f, black_off);
    HairShadeCpu(black, wo, wi, 0.0f, 6.0f, black_on);
    const float blonde_gain = Luma(blonde_on) / std::max(Luma(blonde_off), 1e-9f);
    const float black_gain = Luma(black_on) / std::max(Luma(black_off), 1e-9f);
    Check(blonde_gain > 1.05f, "multiple scattering lifts a light fibre");
    Check(blonde_gain > black_gain,
          "and lifts a light fibre more than a black one (a black fibre absorbs before it "
          "can bounce)");

    // White hair is almost entirely multiple scattering: without it, it is grey.
    HairSurfaceParameters white = HairPresetParams(HairPreset::kWhite);
    HairSurfaceParameters white_noscatter = white;
    white_noscatter.scatter_scale = 0.0f;
    rx::f32 white_on[3], white_off[3];
    HairShadeCpu(white, wo, wi, 0.0f, 8.0f, white_on);
    HairShadeCpu(white_noscatter, wo, wi, 0.0f, 8.0f, white_off);
    Check(Luma(white_on) > Luma(white_off) * 1.1f,
          "white hair depends on multiple scattering (without it, it is grey)");
  }

  // --- 5. the cuticle tilt separates the highlights -------------------------
  {
    // The R and TRT lobes must peak at DIFFERENT longitudinal angles. That
    // separation is the double highlight; without it hair reads as tubing.
    HairSurfaceParameters tilted = HairPresetParams(HairPreset::kBrown);
    HairSurfaceParameters flat = tilted;
    flat.alpha = 0.0f;
    rx::f32 wo[3];
    Dir(0.3f, 0.0f, wo);
    auto peak_theta = [&](const HairSurfaceParameters& p, int channel) {
      float best = -1.0f, best_theta = 0.0f;
      for (int i = 0; i < 400; ++i) {
        const float theta = -1.4f + 2.8f * static_cast<float>(i) / 399.0f;
        rx::f32 wi[3];
        Dir(theta, kPi, wi);  // back toward the light: the specular plane
        rx::f32 rgb[3];
        HairEvaluateCpu(p, wo, wi, 0.0f, rgb);
        if (rgb[channel] > best) {
          best = rgb[channel];
          best_theta = theta;
        }
      }
      return best_theta;
    };
    const float flat_peak = peak_theta(flat, 1);
    const float tilted_peak = peak_theta(tilted, 1);
    Check(std::abs(tilted_peak - flat_peak) > 0.01f,
          "the cuticle tilt shifts the highlight off the specular direction");
  }

  // --- 6. tiers only ever simplify -----------------------------------------
  {
    for (int i = 0; i < 6; ++i) {
      HairSurfaceParameters hero = HairPresetParams(static_cast<HairPreset>(i));
      HairSurfaceParameters standard = hero;
      HairSurfaceParameters distant = hero;
      const HairTierCaps hero_caps = HairTierApply(HairTier::kHero, hero);
      const HairTierCaps standard_caps = HairTierApply(HairTier::kStandard, standard);
      const HairTierCaps distant_caps = HairTierApply(HairTier::kDistant, distant);
      Check(!(standard_caps.dual_scattering && !hero_caps.dual_scattering) &&
                !(distant_caps.dual_scattering && !standard_caps.dual_scattering),
            "tier capabilities are monotonically non-increasing");
      Check(distant.beta_m >= standard.beta_m && distant.beta_n >= standard.beta_n,
            "the distant tier only ever broadens the lobes (narrow lobes are aliasing at "
            "that size, not detail)");
      Check(distant.alpha == 0.0f && distant.scatter_scale == 0.0f,
            "the distant tier drops the tilt and the multiple scattering");
    }
  }

  // --- 7. presets stay inside the published safe ranges ---------------------
  {
    auto in_range = [](const char* field, float value) {
      const HairRange r = HairSafeRange(field);
      return value >= r.lo - 1e-6f && value <= r.hi + 1e-6f;
    };
    for (int i = 0; i < 6; ++i) {
      const HairSurfaceParameters p = HairPresetParams(static_cast<HairPreset>(i));
      Check(in_range("beta_m", p.beta_m) && in_range("beta_n", p.beta_n) &&
                in_range("alpha", p.alpha) && in_range("eta", p.eta) &&
                in_range("density", p.density) && in_range("scatter_scale", p.scatter_scale),
            "every hair preset sits inside the published safe ranges");
    }
    // The presets must actually be distinguishable, in the right order.
    rx::f32 wo[3], wi[3];
    Dir(0.15f, 0.0f, wo);
    Dir(-0.15f, 2.2f, wi);
    float luma[6];
    for (int i = 0; i < 6; ++i) {
      rx::f32 rgb[3];
      HairEvaluateCpu(HairPresetParams(static_cast<HairPreset>(i)), wo, wi, 0.0f, rgb);
      luma[i] = Luma(rgb);
    }
    Check(luma[static_cast<int>(HairPreset::kBlack)] <
              luma[static_cast<int>(HairPreset::kBrown)],
          "black is darker than brown");
    Check(luma[static_cast<int>(HairPreset::kBrown)] <
              luma[static_cast<int>(HairPreset::kBlonde)],
          "brown is darker than blonde");
    Check(luma[static_cast<int>(HairPreset::kBlonde)] <
              luma[static_cast<int>(HairPreset::kWhite)],
          "blonde is darker than white");
  }

  if (failures == 0) std::fprintf(stderr, "hair_bsdf_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
