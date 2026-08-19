#include "render/pipeline/human_material.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <cstring>

// Regression tests for the character surface model. The contract these defend
// is the one the whole thing rests on:
//
//   1. The NEUTRAL parameter set is bit-comparable to the engine's stock
//      Lambert + GGX. If it stops being, then "turn the character model on"
//      silently re-shades every material that opted in, and no fit made before
//      the change means anything after it.
//   2. Every control is independent and monotonic in its own direction.
//   3. Softening the terminator moves light, it does not create it.
//   4. The quality tiers only ever SIMPLIFY - a LOD transition must not be
//      able to change what a material is.
//
// The CPU mirror in human_material.cc is what is exercised here; it is kept
// equivalent to render/shaders/human_brdf.hlsli by construction, and this test
// is what makes that claim checkable without a GPU.

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "human_brdf_test: FAIL: %s\n", message);
  ++failures;
}

struct V3 {
  rx::f32 x, y, z;
};

V3 Norm(V3 v) {
  rx::f32 l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  return {v.x / l, v.y / l, v.z / l};
}

V3 Spherical(rx::f32 theta, rx::f32 phi) {
  return {std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
}

rx::f32 Sum(const rx::f32 v[3]) { return v[0] + v[1] + v[2]; }

constexpr rx::f32 kPi = 3.14159265358979323846f;

}  // namespace

int main() {
  using namespace rx::render;

  const rx::f32 base[3] = {0.62f, 0.44f, 0.35f};
  const rx::f32 f0[3] = {0.04f, 0.04f, 0.04f};
  const rx::f32 n[3] = {0.0f, 1.0f, 0.0f};

  // --- 1. neutral parity ----------------------------------------------------
  {
    const HumanSurfaceParameters neutral = HumanNeutral();
    rx::f32 worst = 0.0f;
    for (int ri = 1; ri <= 8; ++ri) {
      const rx::f32 roughness = static_cast<rx::f32>(ri) / 8.0f;
      for (int vi = 0; vi < 12; ++vi) {
        const V3 vv = Spherical(static_cast<rx::f32>(vi) * (kPi * 0.5f / 12.0f) + 0.02f, 0.7f);
        for (int li = 0; li < 24; ++li) {
          const V3 ll = Spherical(static_cast<rx::f32>(li) * (kPi * 0.5f / 12.0f) + 0.02f,
                                  static_cast<rx::f32>(li) * 0.53f);
          const rx::f32 v[3] = {vv.x, vv.y, vv.z};
          const rx::f32 l[3] = {ll.x, ll.y, ll.z};
          // A NON-ZERO solid angle on purpose: the neutral set has to reproduce
          // the stock path under an area light too, which it only does because
          // light_shape_response is a material control that neutral leaves at
          // zero. This is the check that caught the shape-aware widening
          // silently re-shading every highlight in the frame.
          const HumanBrdfSample h =
              HumanEvaluateCpu(neutral, base, roughness, f0, n, n, n, v, l, 0.4f, 0.0f);
          const HumanBrdfSample s = StockBrdfCpu(base, roughness, f0, n, v, l);
          for (int c = 0; c < 3; ++c) {
            worst = std::max(worst, std::abs(h.diffuse[c] - s.diffuse[c]));
            worst = std::max(worst, std::abs(h.specular[c] - s.specular[c]));
          }
          Check(Sum(h.transmission) == 0.0f, "neutral transmission is exactly zero");
        }
      }
    }
    Check(worst < 1e-6f, "the neutral set reproduces stock Lambert + GGX");
    if (worst >= 1e-6f) std::fprintf(stderr, "  worst neutral delta: %g\n", worst);
  }

  const rx::f32 v_side[3] = {0.5f, 0.6f, 0.62f};
  const rx::f32 l_45[3] = {0.707f, 0.707f, 0.0f};
  const rx::f32 l_graze[3] = {0.985f, 0.174f, 0.0f};

  // --- 2. each control is independent and monotone --------------------------
  {
    HumanSurfaceParameters p = HumanNeutral();
    const HumanBrdfSample zero =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_side, l_graze, 0.0f, 0.0f);
    p.diffuse_fresnel_peak = 0.5f;
    const HumanBrdfSample lifted =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_side, l_graze, 0.0f, 0.0f);
    Check(Sum(lifted.diffuse) > Sum(zero.diffuse),
          "diffuse Fresnel lifts the grazing diffuse");
    Check(std::abs(Sum(lifted.specular) - Sum(zero.specular)) < 1e-7f,
          "diffuse Fresnel does not touch the specular lobe");

    p = HumanNeutral();
    p.retroreflection_peak = 1.0f;
    const HumanBrdfSample retro =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_side, l_graze, 0.0f, 0.0f);
    Check(Sum(retro.diffuse) > Sum(zero.diffuse), "retroreflection lifts the back-scatter");
    Check(std::abs(Sum(retro.specular) - Sum(zero.specular)) < 1e-7f,
          "retroreflection does not touch the specular lobe");

    p = HumanNeutral();
    p.specular_fresnel_falloff = 2.0f;  // a slower falloff -> more grazing reflectance
    const HumanBrdfSample fres =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_side, l_graze, 0.0f, 0.0f);
    Check(Sum(fres.specular) > Sum(zero.specular),
          "a lower specular Fresnel exponent raises grazing reflectance");
    Check(std::abs(Sum(fres.diffuse) - Sum(zero.diffuse)) < 1e-7f,
          "the specular Fresnel exponent does not touch the diffuse lobe");
  }

  // --- 3. the terminator moves light, it does not create it -----------------
  {
    HumanSurfaceParameters hard = HumanNeutral();
    HumanSurfaceParameters soft = HumanNeutral();
    soft.smooth_terminator_amount = 1.0f;
    soft.smooth_terminator_length = 0.25f;

    // Hemispherical integral of the diffuse lobe: the wrap is energy
    // normalized, so softening must not raise the total.
    auto integrate = [&](const HumanSurfaceParameters& p) {
      double total = 0.0;
      const int steps = 256;
      for (int i = 0; i < steps; ++i) {
        // Sample the FULL sphere: the soft terminator reaches past the horizon,
        // and an upper-hemisphere-only integral would miss exactly the energy
        // the normalization is there to account for.
        const rx::f32 theta = (static_cast<rx::f32>(i) + 0.5f) * kPi / static_cast<rx::f32>(steps);
        const V3 ll = Spherical(theta, 0.0f);
        const rx::f32 l[3] = {ll.x, ll.y, ll.z};
        const HumanBrdfSample s =
            HumanEvaluateCpu(p, base, 0.5f, f0, n, n, n, v_side, l, 0.0f, 0.0f);
        total += static_cast<double>(s.diffuse[0]) * std::sin(theta);
      }
      return total * (kPi / steps) * 2.0 * kPi;
    };
    const double hard_energy = integrate(hard);
    const double soft_energy = integrate(soft);
    Check(soft_energy <= hard_energy * 1.02,
          "softening the terminator does not add diffuse energy");
    Check(soft_energy > hard_energy * 0.75,
          "softening the terminator does not throw diffuse energy away");

    // ... and it must actually soften: light past the geometric terminator.
    const rx::f32 l_past[3] = {0.995f, -0.1f, 0.0f};  // just below the horizon
    const HumanBrdfSample dark =
        HumanEvaluateCpu(hard, base, 0.5f, f0, n, n, n, v_side, l_past, 0.0f, 0.0f);
    const HumanBrdfSample lit =
        HumanEvaluateCpu(soft, base, 0.5f, f0, n, n, n, v_side, l_past, 0.0f, 0.0f);
    Check(Sum(dark.diffuse) == 0.0f, "the hard terminator is hard");
    Check(Sum(lit.diffuse) > 0.0f, "the soft terminator reaches past the horizon");
    Check(Sum(lit.specular) == Sum(dark.specular),
          "the terminator control never widens the specular lobe");
  }

  // --- 4. the second lobe blends, it does not add ---------------------------
  {
    HumanSurfaceParameters single = HumanNeutral();
    HumanSurfaceParameters dual = HumanNeutral();
    dual.secondary_specular_weight = 0.5f;
    dual.secondary_roughness_scale = 3.0f;
    const HumanBrdfSample a =
        HumanEvaluateCpu(single, base, 0.3f, f0, n, n, n, v_side, l_45, 0.0f, 0.0f);
    const HumanBrdfSample b =
        HumanEvaluateCpu(dual, base, 0.3f, f0, n, n, n, v_side, l_45, 0.0f, 0.0f);
    Check(std::abs(Sum(a.diffuse) - Sum(b.diffuse)) < 1e-7f,
          "the second specular lobe does not touch the diffuse lobe");

    // Integrated over the hemisphere the two lobes must carry the same energy;
    // the tail redistributes the core, it does not add a second highlight.
    auto spec_energy = [&](const HumanSurfaceParameters& p) {
      double total = 0.0;
      const int steps = 512;
      for (int i = 0; i < steps; ++i) {
        const rx::f32 theta =
            (static_cast<rx::f32>(i) + 0.5f) * (kPi * 0.5f) / static_cast<rx::f32>(steps);
        for (int j = 0; j < 32; ++j) {
          const rx::f32 phi = static_cast<rx::f32>(j) * (2.0f * kPi / 32.0f);
          const V3 ll = Spherical(theta, phi);
          const rx::f32 l[3] = {ll.x, ll.y, ll.z};
          const HumanBrdfSample s =
              HumanEvaluateCpu(p, base, 0.3f, f0, n, n, n, v_side, l, 0.0f, 0.0f);
          total += static_cast<double>(s.specular[0]) * std::sin(theta);
        }
      }
      return total;
    };
    // The guarantee a lerp gives is that the blend lands BETWEEN the two lobes,
    // never above either. That is precisely what rules out "the tail is a
    // second highlight bolted on top" - which is the failure mode of adding
    // the lobes instead of blending them. (The two are not equal in energy:
    // the broader lobe loses more to Smith masking, which is real.)
    HumanSurfaceParameters tail_only = HumanNeutral();
    tail_only.secondary_specular_weight = 1.0f;
    tail_only.secondary_roughness_scale = 3.0f;
    const double e_single = spec_energy(single);
    const double e_dual = spec_energy(dual);
    const double e_tail = spec_energy(tail_only);
    const double lo = std::min(e_single, e_tail);
    const double hi = std::max(e_single, e_tail);
    Check(e_dual >= lo - hi * 1e-6 && e_dual <= hi + hi * 1e-6,
          "the second lobe redistributes specular energy rather than adding it");
    Check(e_dual > lo && e_dual < hi,
          "a half-weight blend actually sits between the two lobes");
  }

  // --- 5. separate diffuse and specular normals -----------------------------
  {
    HumanSurfaceParameters p = HumanNeutral();
    const rx::f32 ns[3] = {0.30f, 0.95f, 0.0f};  // a "sweat" normal, tilted
    const HumanBrdfSample shared_n =
        HumanEvaluateCpu(p, base, 0.25f, f0, n, n, n, v_side, l_45, 0.0f, 0.0f);
    const HumanBrdfSample split =
        HumanEvaluateCpu(p, base, 0.25f, f0, n, n, ns, v_side, l_45, 0.0f, 0.0f);
    Check(std::abs(Sum(shared_n.diffuse) - Sum(split.diffuse)) < 1e-7f,
          "a specular-only normal leaves the diffuse lobe alone");
    Check(std::abs(Sum(shared_n.specular) - Sum(split.specular)) > 1e-6f,
          "a specular-only normal moves the highlight");
  }

  // --- 6. transmission ------------------------------------------------------
  {
    HumanSurfaceParameters p = HumanPreset(HumanRegion::kSkin);
    const rx::f32 l_back[3] = {0.2f, -0.9f, 0.39f};
    const rx::f32 v_front[3] = {0.0f, 1.0f, 0.0f};
    const HumanBrdfSample thin =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_front, l_back, 0.0f, 0.0005f);
    const HumanBrdfSample thick =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_front, l_back, 0.0f, 0.05f);
    Check(Sum(thin.transmission) > Sum(thick.transmission),
          "a thicker part transmits less (Beer-Lambert)");
    p.transmission = 0.0f;
    const HumanBrdfSample opaque =
        HumanEvaluateCpu(p, base, 0.4f, f0, n, n, n, v_front, l_back, 0.0f, 0.0005f);
    Check(Sum(opaque.transmission) == 0.0f, "transmission 0 means opaque");
  }

  // --- 7. light-shape widening ---------------------------------------------
  {
    HumanSurfaceParameters p = HumanNeutral();
    // The response is a MATERIAL control, off in the neutral set on purpose:
    // the engine's stock path treats every light as punctual, so leaving this
    // on by default would break neutral parity on every highlight in the frame.
    p.light_shape_response = 1.0f;
    // Sit the light exactly on the mirror direction, where the lobe peaks: a
    // wider lobe carries the same energy over more solid angle, so the peak is
    // where the widening is unambiguous.
    const V3 vn = Norm({v_side[0], v_side[1], v_side[2]});
    const rx::f32 l_mirror[3] = {-vn.x, vn.y, -vn.z};
    const rx::f32 v_unit[3] = {vn.x, vn.y, vn.z};
    // Roughness 0.25, not something mirror-smooth: the engine's D_GGX carries a
    // 1e-7 denominator guard that saturates the peak below roughness ~0.1, so a
    // test written down there would be measuring the guard.
    const HumanBrdfSample punctual =
        HumanEvaluateCpu(p, base, 0.25f, f0, n, n, n, v_unit, l_mirror, 0.0f, 0.0f);
    const HumanBrdfSample area =
        HumanEvaluateCpu(p, base, 0.25f, f0, n, n, n, v_unit, l_mirror, 0.35f, 0.0f);
    Check(Sum(area.specular) < Sum(punctual.specular),
          "a light with real solid angle cannot produce a tighter core than its own image");
    const HumanBrdfSample bigger =
        HumanEvaluateCpu(p, base, 0.25f, f0, n, n, n, v_unit, l_mirror, 1.2f, 0.0f);
    Check(Sum(bigger.specular) < Sum(area.specular),
          "a larger emitter widens the lobe further");
  }

  // --- 8. tiers only ever simplify -----------------------------------------
  {
    for (int r = 0; r < 8; ++r) {
      const HumanRegion region = static_cast<HumanRegion>(r);
      HumanSurfaceParameters hero = HumanPreset(region);
      HumanSurfaceParameters standard = HumanPreset(region);
      HumanSurfaceParameters distant = HumanPreset(region);
      const HumanTierCaps hero_caps = HumanTierApply(HumanTier::kHero, hero);
      const HumanTierCaps standard_caps = HumanTierApply(HumanTier::kStandard, standard);
      const HumanTierCaps distant_caps = HumanTierApply(HumanTier::kDistant, distant);
      Check(standard.secondary_specular_weight <= hero.secondary_specular_weight &&
                distant.secondary_specular_weight <= standard.secondary_specular_weight,
            "the second lobe only ever gets cheaper down the tiers");
      Check(standard.transmission <= hero.transmission &&
                distant.transmission <= standard.transmission,
            "transmission only ever gets cheaper down the tiers");
      Check(distant.diffuse_fresnel_peak == 0.0f && distant.retroreflection_peak == 0.0f &&
                distant.smooth_terminator_amount == 0.0f,
            "the distant tier collapses to the neutral model");
      Check(!(standard_caps.dual_specular && !hero_caps.dual_specular) &&
                !(distant_caps.dual_specular && !standard_caps.dual_specular),
            "tier capabilities are monotonically non-increasing");
      Check(!distant_caps.residual && !distant_caps.eye_refraction,
            "the distant tier runs neither residual correction nor eye refraction");
    }
    Check(HumanTierForScreenHeight(1080.0f) == HumanTier::kHero, "a full-screen head is hero");
    Check(HumanTierForScreenHeight(120.0f) == HumanTier::kStandard,
          "a gameplay-distance head is standard");
    Check(HumanTierForScreenHeight(12.0f) == HumanTier::kDistant, "a tiny head is distant");
  }

  // --- 9. the presets stay inside the published safe ranges -----------------
  {
    auto in_range = [](const char* field, rx::f32 value) {
      const HumanRange r = HumanSafeRange(field);
      return value >= r.lo - 1e-6f && value <= r.hi + 1e-6f;
    };
    for (int r = 0; r < 8; ++r) {
      const HumanSurfaceParameters p = HumanPreset(static_cast<HumanRegion>(r));
      Check(in_range("diffuse_fresnel_peak", p.diffuse_fresnel_peak) &&
                in_range("diffuse_fresnel_falloff", p.diffuse_fresnel_falloff) &&
                in_range("retroreflection_peak", p.retroreflection_peak) &&
                in_range("smooth_terminator_amount", p.smooth_terminator_amount) &&
                in_range("smooth_terminator_length", p.smooth_terminator_length) &&
                in_range("specular_fresnel_falloff", p.specular_fresnel_falloff) &&
                in_range("secondary_roughness_scale", p.secondary_roughness_scale) &&
                in_range("secondary_specular_weight", p.secondary_specular_weight) &&
                in_range("mean_free_path", p.mean_free_path) &&
                in_range("transmission", p.transmission) &&
                in_range("thickness_scale", p.thickness_scale) &&
                in_range("corneal_wetness", p.corneal_wetness) &&
                in_range("cavity_occlusion", p.cavity_occlusion) &&
                in_range("iris_depth", p.iris_depth) && in_range("cornea_ior", p.cornea_ior),
            "every region preset sits inside the published safe ranges");
    }
  }

  // --- 10. authored <-> resolved round trip ---------------------------------
  {
    HumanSurfaceParameters original = HumanPreset(HumanRegion::kTeeth);
    original.residual_weight = 0.37f;
    rx::asset::Material::HumanParams authored;
    HumanStore(original, authored);
    const HumanSurfaceParameters back = HumanResolve(authored);
    Check(back.region == original.region && back.diffuse_fresnel_peak == original.diffuse_fresnel_peak &&
              back.retroreflection_peak == original.retroreflection_peak &&
              back.smooth_terminator_length == original.smooth_terminator_length &&
              back.secondary_specular_weight == original.secondary_specular_weight &&
              back.mean_free_path == original.mean_free_path &&
              back.transmission == original.transmission &&
              back.corneal_wetness == original.corneal_wetness &&
              back.iris_depth == original.iris_depth &&
              back.residual_weight == original.residual_weight,
          "HumanStore / HumanResolve round-trip every field");
  }

  if (failures == 0) std::fprintf(stderr, "human_brdf_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
