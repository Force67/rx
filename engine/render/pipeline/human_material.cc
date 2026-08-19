#include "render/pipeline/human_material.h"

#include <cmath>
#include <cstring>

namespace rx::render {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;

struct V3 {
  f32 x, y, z;
};
V3 Load(const f32 v[3]) { return {v[0], v[1], v[2]}; }
f32 Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Norm(V3 a) {
  f32 len = std::sqrt(std::max(Dot(a, a), 1e-20f));
  return {a.x / len, a.y / len, a.z / len};
}
f32 Sat(f32 x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

f32 D_GGX(f32 ndh, f32 a) {
  f32 a2 = a * a;
  f32 d = ndh * ndh * (a2 - 1.0f) + 1.0f;
  return a2 / std::max(kPi * d * d, 1e-7f);
}
f32 V_Smith(f32 ndv, f32 ndl, f32 a) {
  f32 a2 = a * a;
  f32 gv = ndl * std::sqrt(ndv * ndv * (1.0f - a2) + a2);
  f32 gl = ndv * std::sqrt(ndl * ndl * (1.0f - a2) + a2);
  return 0.5f / std::max(gv + gl, 1e-5f);
}

}  // namespace

HumanSurfaceParameters HumanNeutral() { return HumanSurfaceParameters{}; }

HumanSurfaceParameters HumanPreset(HumanRegion region) {
  HumanSurfaceParameters p;
  p.region = region;
  switch (region) {
    case HumanRegion::kSkin:
      // Skin's grazing diffuse lifts (the stratum corneum is a rough dielectric
      // boundary, so more light survives the two crossings at grazing than
      // Lambert predicts), it retroreflects noticeably, and its terminator is
      // soft because the light that "should" have stopped at N.L = 0 entered a
      // few hundred microns earlier and came back out. The broad specular tail
      // is the oil film over the microrelief; the tight core is the relief
      // itself.
      p.diffuse_fresnel_peak = 0.35f;
      p.diffuse_fresnel_falloff = 4.0f;
      p.diffuse_fresnel_tangent_falloff = 4.0f;
      p.retroreflection_peak = 0.55f;
      p.retroreflection_falloff = 4.0f;
      p.retroreflection_tangent_falloff = 4.0f;
      p.smooth_terminator_amount = 0.65f;
      p.smooth_terminator_length = 0.22f;
      p.specular_fresnel_falloff = 4.4f;
      p.secondary_roughness_scale = 2.6f;
      p.secondary_specular_weight = 0.28f;
      p.light_shape_response = 1.0f;
      p.mean_free_path = 0.0011f;
      p.transmission = 0.35f;
      p.transmission_tint[0] = 1.0f;
      p.transmission_tint[1] = 0.32f;
      p.transmission_tint[2] = 0.18f;
      p.thickness_scale = 0.012f;
      break;
    case HumanRegion::kLips:
      // Thinner epidermis over a dense capillary bed: shorter mean free path,
      // redder transport, wetter surface, harder terminator than cheek skin.
      p = HumanPreset(HumanRegion::kSkin);
      p.region = region;
      p.mean_free_path = 0.0007f;
      p.transmission = 0.55f;
      p.transmission_tint[1] = 0.22f;
      p.transmission_tint[2] = 0.16f;
      p.smooth_terminator_amount = 0.5f;
      p.corneal_wetness = 0.25f;
      p.secondary_specular_weight = 0.18f;
      p.thickness_scale = 0.006f;
      break;
    case HumanRegion::kTeeth:
      // Enamel is a translucent shell over opaque dentin: short-range, almost
      // achromatic diffusion, a hard specular core, and a strong diffuse
      // Fresnel that is exactly what makes a tooth read as glassy rather than
      // as painted bone. Grazing retroreflection carries the incisal edge.
      p.diffuse_fresnel_peak = 0.6f;
      p.diffuse_fresnel_falloff = 3.2f;
      p.diffuse_fresnel_tangent_falloff = 3.2f;
      p.retroreflection_peak = 0.75f;
      p.retroreflection_falloff = 3.0f;
      p.retroreflection_tangent_falloff = 3.0f;
      p.smooth_terminator_amount = 0.35f;
      p.smooth_terminator_length = 0.12f;
      p.specular_fresnel_falloff = 5.0f;
      p.secondary_roughness_scale = 4.0f;
      p.secondary_specular_weight = 0.15f;
      p.light_shape_response = 1.0f;
      p.mean_free_path = 0.0006f;
      p.transmission = 0.45f;
      p.transmission_tint[0] = 0.95f;
      p.transmission_tint[1] = 0.9f;
      p.transmission_tint[2] = 0.82f;
      p.thickness_scale = 0.004f;
      p.corneal_wetness = 0.35f;  // saliva film
      p.cavity_occlusion = 0.55f;
      break;
    case HumanRegion::kGums:
      p.light_shape_response = 1.0f;
      p.diffuse_fresnel_peak = 0.3f;
      p.diffuse_fresnel_falloff = 4.0f;
      p.diffuse_fresnel_tangent_falloff = 4.0f;
      p.retroreflection_peak = 0.4f;
      p.smooth_terminator_amount = 0.7f;
      p.smooth_terminator_length = 0.28f;
      p.mean_free_path = 0.0009f;
      p.transmission = 0.5f;
      p.transmission_tint[0] = 1.0f;
      p.transmission_tint[1] = 0.24f;
      p.transmission_tint[2] = 0.2f;
      p.thickness_scale = 0.005f;
      p.corneal_wetness = 0.4f;
      p.cavity_occlusion = 0.7f;
      break;
    case HumanRegion::kSclera:
      // Wet, shallow, near-white scattering; the vessels come from the albedo.
      p.diffuse_fresnel_peak = 0.4f;
      p.diffuse_fresnel_falloff = 3.5f;
      p.diffuse_fresnel_tangent_falloff = 3.5f;
      p.retroreflection_peak = 0.3f;
      p.smooth_terminator_amount = 0.55f;
      p.smooth_terminator_length = 0.18f;
      p.secondary_roughness_scale = 3.0f;
      p.secondary_specular_weight = 0.1f;
      p.light_shape_response = 1.0f;
      p.mean_free_path = 0.0004f;
      p.transmission = 0.15f;
      p.transmission_tint[0] = 1.0f;
      p.transmission_tint[1] = 0.75f;
      p.transmission_tint[2] = 0.7f;
      p.thickness_scale = 0.003f;
      p.corneal_wetness = 0.85f;
      break;
    case HumanRegion::kCornea:
      // The shell itself contributes almost no diffuse; it refracts and it
      // reflects. Wetness carries the tear film's mirror lobe, and a cornea is
      // the surface where a light's shape is most visible - the catchlight IS
      // the emitter's image.
      p.light_shape_response = 1.0f;
      p.diffuse_fresnel_peak = 0.0f;
      p.retroreflection_peak = 0.0f;
      p.smooth_terminator_amount = 0.0f;
      p.specular_fresnel_falloff = 5.0f;
      p.secondary_specular_weight = 0.0f;
      p.corneal_wetness = 1.0f;
      p.transmission = 0.0f;
      break;
    case HumanRegion::kIris:
      p.light_shape_response = 1.0f;
      // Shaded BEHIND the cornea: it gets no wet lobe of its own (the cornea
      // above it owns the reflection) but keeps a soft, deep diffuse.
      p.diffuse_fresnel_peak = 0.25f;
      p.diffuse_fresnel_falloff = 4.0f;
      p.diffuse_fresnel_tangent_falloff = 4.0f;
      p.retroreflection_peak = 0.2f;
      p.smooth_terminator_amount = 0.4f;
      p.smooth_terminator_length = 0.15f;
      p.mean_free_path = 0.0003f;
      p.transmission = 0.1f;
      p.thickness_scale = 0.002f;
      break;
    case HumanRegion::kTearline:
      p.light_shape_response = 1.0f;
      p.diffuse_fresnel_peak = 0.0f;
      p.retroreflection_peak = 0.0f;
      p.corneal_wetness = 1.0f;
      p.transmission = 0.2f;
      p.thickness_scale = 0.0006f;
      break;
  }
  return p;
}

HumanTierCaps HumanTierApply(HumanTier tier, HumanSurfaceParameters& params) {
  HumanTierCaps caps;
  switch (tier) {
    case HumanTier::kHero:
      break;
    case HumanTier::kStandard:
      // The second lobe is the first thing to go: it is the most expensive
      // control per pixel of visible difference at gameplay distance.
      caps.dual_specular = false;
      caps.residual = false;
      caps.full_sss = false;
      params.secondary_specular_weight = 0.0f;
      params.residual_weight = 0.0f;
      // Transmission survives but stops sampling a thickness map's detail.
      params.transmission *= 0.75f;
      break;
    case HumanTier::kDistant:
      // Collapse to the neutral model. At this size the controls are all
      // sub-pixel, and keeping them alive only costs stability under
      // dynamic resolution.
      caps = {false, false, false, false, false, false};
      params.diffuse_fresnel_peak = 0.0f;
      params.retroreflection_peak = 0.0f;
      params.smooth_terminator_amount = 0.0f;
      params.secondary_specular_weight = 0.0f;
      params.transmission = 0.0f;
      params.corneal_wetness = 0.0f;
      params.residual_weight = 0.0f;
      params.light_shape_response = 0.0f;
      break;
  }
  return caps;
}

HumanTier HumanTierForScreenHeight(f32 pixels) {
  // A head under ~64 px cannot resolve a terminator softening of 0.2 cosine
  // units; over ~360 px the second specular lobe is visibly missing.
  if (pixels >= 360.0f) return HumanTier::kHero;
  if (pixels >= 64.0f) return HumanTier::kStandard;
  return HumanTier::kDistant;
}

HumanRange HumanSafeRange(const char* field) {
  auto is = [&](const char* n) { return std::strcmp(field, n) == 0; };
  // Outside these the model stops being energy-sane: a diffuse Fresnel peak
  // above 1 can more than double the grazing diffuse, a terminator length above
  // ~0.5 wraps light onto the far side of a sphere, a secondary roughness scale
  // below 1 makes the "tail" tighter than the core (it is then not a tail), and
  // a wet weight above 1 would remove more energy than the base lobe carries.
  if (is("diffuse_fresnel_peak")) return {-0.5f, 1.0f};
  if (is("diffuse_fresnel_falloff")) return {1.0f, 8.0f};
  if (is("diffuse_fresnel_tangent_falloff")) return {1.0f, 8.0f};
  if (is("retroreflection_peak")) return {0.0f, 2.0f};
  if (is("retroreflection_falloff")) return {1.0f, 8.0f};
  if (is("retroreflection_tangent_falloff")) return {1.0f, 8.0f};
  if (is("smooth_terminator_amount")) return {0.0f, 1.0f};
  if (is("smooth_terminator_length")) return {0.0f, 0.5f};
  if (is("specular_fresnel_falloff")) return {2.0f, 8.0f};
  if (is("secondary_roughness_scale")) return {1.0f, 8.0f};
  if (is("secondary_specular_weight")) return {0.0f, 1.0f};
  if (is("light_shape_response")) return {0.0f, 1.0f};
  if (is("mean_free_path")) return {0.0001f, 0.01f};
  if (is("subsurface_scale")) return {0.1f, 4.0f};
  if (is("transmission")) return {0.0f, 1.0f};
  if (is("extinction_scale")) return {0.1f, 8.0f};
  if (is("thickness_scale")) return {0.0005f, 0.1f};
  if (is("corneal_wetness")) return {0.0f, 1.0f};
  if (is("cavity_occlusion")) return {0.0f, 1.0f};
  if (is("specular_normal_strength")) return {0.0f, 2.0f};
  if (is("iris_depth")) return {0.0f, 0.01f};
  if (is("iris_radius")) return {0.02f, 0.5f};
  if (is("pupil_scale")) return {0.4f, 2.0f};
  if (is("limbal_ring_size")) return {0.0f, 0.2f};
  if (is("limbal_ring_power")) return {0.5f, 8.0f};
  if (is("cornea_ior")) return {1.0f, 1.6f};
  if (is("iris_shadow_depth")) return {0.0f, 1.0f};
  if (is("residual_weight")) return {0.0f, 1.0f};
  return {0.0f, 1.0f};
}

HumanSurfaceParameters HumanResolve(const asset::Material::HumanParams& a) {
  HumanSurfaceParameters p;
  p.region = a.region;
  p.diffuse_fresnel_peak = a.diffuse_fresnel_peak;
  p.diffuse_fresnel_falloff = a.diffuse_fresnel_falloff;
  p.diffuse_fresnel_tangent_falloff = a.diffuse_fresnel_tangent_falloff;
  p.retroreflection_peak = a.retroreflection_peak;
  p.retroreflection_falloff = a.retroreflection_falloff;
  p.retroreflection_tangent_falloff = a.retroreflection_tangent_falloff;
  p.smooth_terminator_amount = a.smooth_terminator_amount;
  p.smooth_terminator_length = a.smooth_terminator_length;
  p.specular_fresnel_falloff = a.specular_fresnel_falloff;
  p.secondary_roughness_scale = a.secondary_roughness_scale;
  p.secondary_specular_weight = a.secondary_specular_weight;
  p.light_shape_response = a.light_shape_response;
  p.mean_free_path = a.mean_free_path;
  p.subsurface_scale = a.subsurface_scale;
  p.transmission = a.transmission;
  std::memcpy(p.transmission_tint, a.transmission_tint, sizeof(f32) * 3);
  p.extinction_scale = a.extinction_scale;
  p.thickness_scale = a.thickness_scale;
  p.corneal_wetness = a.corneal_wetness;
  p.cavity_occlusion = a.cavity_occlusion;
  p.specular_normal_strength = a.specular_normal_strength;
  p.iris_depth = a.iris_depth;
  p.iris_radius = a.iris_radius;
  p.pupil_scale = a.pupil_scale;
  p.limbal_ring_size = a.limbal_ring_size;
  p.limbal_ring_power = a.limbal_ring_power;
  p.cornea_ior = a.cornea_ior;
  p.iris_shadow_depth = a.iris_shadow_depth;
  p.residual_weight = a.residual_weight;
  return p;
}

void HumanStore(const HumanSurfaceParameters& p, asset::Material::HumanParams& a) {
  a.region = p.region;
  a.diffuse_fresnel_peak = p.diffuse_fresnel_peak;
  a.diffuse_fresnel_falloff = p.diffuse_fresnel_falloff;
  a.diffuse_fresnel_tangent_falloff = p.diffuse_fresnel_tangent_falloff;
  a.retroreflection_peak = p.retroreflection_peak;
  a.retroreflection_falloff = p.retroreflection_falloff;
  a.retroreflection_tangent_falloff = p.retroreflection_tangent_falloff;
  a.smooth_terminator_amount = p.smooth_terminator_amount;
  a.smooth_terminator_length = p.smooth_terminator_length;
  a.specular_fresnel_falloff = p.specular_fresnel_falloff;
  a.secondary_roughness_scale = p.secondary_roughness_scale;
  a.secondary_specular_weight = p.secondary_specular_weight;
  a.light_shape_response = p.light_shape_response;
  a.mean_free_path = p.mean_free_path;
  a.subsurface_scale = p.subsurface_scale;
  a.transmission = p.transmission;
  std::memcpy(a.transmission_tint, p.transmission_tint, sizeof(f32) * 3);
  a.extinction_scale = p.extinction_scale;
  a.thickness_scale = p.thickness_scale;
  a.corneal_wetness = p.corneal_wetness;
  a.cavity_occlusion = p.cavity_occlusion;
  a.specular_normal_strength = p.specular_normal_strength;
  a.iris_depth = p.iris_depth;
  a.iris_radius = p.iris_radius;
  a.pupil_scale = p.pupil_scale;
  a.limbal_ring_size = p.limbal_ring_size;
  a.limbal_ring_power = p.limbal_ring_power;
  a.cornea_ior = p.cornea_ior;
  a.iris_shadow_depth = p.iris_shadow_depth;
  a.residual_weight = p.residual_weight;
}

// --- CPU mirror -------------------------------------------------------------

HumanBrdfSample HumanEvaluateCpu(const HumanSurfaceParameters& p, const f32 base_color[3],
                                 f32 roughness, const f32 f0[3], const f32 geometric_n[3],
                                 const f32 nd_in[3], const f32 ns_in[3], const f32 v_in[3],
                                 const f32 l_in[3], f32 solid_angle, f32 thickness) {
  HumanBrdfSample out;
  V3 ng = Load(geometric_n), nd = Load(nd_in), ns = Load(ns_in);
  V3 v = Load(v_in), l = Load(l_in);

  f32 ndl_d = Dot(nd, l);
  f32 ndv_d = std::max(Dot(nd, v), 1e-4f);
  f32 ndl_g = Dot(ng, l);

  // diffuse cosine with the energy-normalized wrap
  f32 cos_d = Sat(ndl_d);
  if (p.smooth_terminator_amount > 0.0f) {
    f32 w = std::max(p.smooth_terminator_length, 0.0f);
    f32 soft = Sat((ndl_d + w) / ((1.0f + w) * (1.0f + w)));
    f32 gate = Sat(ndl_g * 4.0f + 1.0f);
    f32 amount = Sat(p.smooth_terminator_amount) * gate;
    cos_d = cos_d + (soft - cos_d) * amount;
  }

  if (cos_d > 0.0f) {
    V3 hd = Norm(Add(l, v));
    f32 ldh = Sat(Dot(l, hd));
    f32 gv = std::pow(Sat(1.0f - ndv_d), std::max(p.diffuse_fresnel_falloff, 1e-2f));
    f32 gl = std::pow(Sat(1.0f - Sat(ndl_d)),
                      std::max(p.diffuse_fresnel_tangent_falloff, 1e-2f));
    f32 fresnel = (1.0f + p.diffuse_fresnel_peak * gv) * (1.0f + p.diffuse_fresnel_peak * gl);
    f32 fd90 = p.retroreflection_peak * ldh * ldh;
    f32 rv = 1.0f + fd90 * std::pow(Sat(1.0f - ndv_d),
                                    std::max(p.retroreflection_tangent_falloff, 1e-2f));
    f32 rl = 1.0f + fd90 * std::pow(Sat(1.0f - Sat(ndl_d)),
                                    std::max(p.retroreflection_falloff, 1e-2f));
    f32 shaping = std::max(fresnel * rv * rl, 0.0f);
    for (int c = 0; c < 3; ++c) out.diffuse[c] = base_color[c] * (1.0f / kPi) * shaping * cos_d;
  }

  f32 ndl_s = Dot(ns, l);
  if (ndl_s > 0.0f) {
    f32 ndl = Sat(ndl_s);
    f32 ndv = std::max(Dot(ns, v), 1e-4f);
    V3 h = Norm(Add(l, v));
    f32 ndh = Sat(Dot(ns, h));
    f32 vdh = Sat(Dot(v, h));

    f32 rough = roughness;
    solid_angle *= Sat(p.light_shape_response);
    if (solid_angle > 0.0f) {
      f32 widen = Sat(std::sqrt(solid_angle / kPi) * 0.5f);
      rough = Sat(std::sqrt(roughness * roughness + widen * widen));
    }
    f32 a1 = std::max(rough * rough, 1e-5f);
    f32 core = D_GGX(ndh, a1) * V_Smith(ndv, ndl, a1);
    f32 lobe = core;
    if (p.secondary_specular_weight > 0.0f) {
      f32 rough2 = Sat(rough * std::max(p.secondary_roughness_scale, 1e-2f));
      f32 a2 = std::max(rough2 * rough2, 1e-5f);
      f32 tail = D_GGX(ndh, a2) * V_Smith(ndv, ndl, a2);
      f32 w = Sat(p.secondary_specular_weight);
      lobe = core + (tail - core) * w;
    }
    f32 fpow = std::pow(Sat(1.0f - vdh), std::max(p.specular_fresnel_falloff, 1e-2f));
    for (int c = 0; c < 3; ++c) {
      f32 f = f0[c] + (1.0f - f0[c]) * fpow;
      out.specular[c] = lobe * f * ndl;
    }
    if (p.corneal_wetness > 0.0f) {
      f32 wa = std::max(0.02f * 0.02f, 1e-5f);
      f32 wf = (0.02f + 0.98f * std::pow(Sat(1.0f - vdh), 5.0f)) * Sat(p.corneal_wetness);
      f32 wet = D_GGX(ndh, wa) * V_Smith(ndv, ndl, wa) * ndl;
      for (int c = 0; c < 3; ++c) {
        out.specular[c] = out.specular[c] * (1.0f - wf) + wet * wf;
        out.diffuse[c] *= (1.0f - wf);
      }
    }
  }

  if (p.transmission > 0.0f && ndl_d < 0.35f) {
    f32 optical =
        std::max(p.extinction_scale * thickness / std::max(p.mean_free_path * p.subsurface_scale, 1e-4f), 0.0f);
    f32 attenuation = std::exp(-optical);
    f32 back = Sat(Dot(v, {-l.x, -l.y, -l.z}));
    f32 lobe = back * back * Sat(0.35f - ndl_d) / 0.35f;
    for (int c = 0; c < 3; ++c) {
      out.transmission[c] =
          base_color[c] * p.transmission_tint[c] * p.transmission * attenuation * lobe;
    }
  }
  return out;
}

HumanBrdfSample StockBrdfCpu(const f32 base_color[3], f32 roughness, const f32 f0[3],
                             const f32 n_in[3], const f32 v_in[3], const f32 l_in[3]) {
  HumanBrdfSample out;
  V3 n = Load(n_in), v = Load(v_in), l = Load(l_in);
  f32 ndl = Sat(Dot(n, l));
  f32 ndv = std::max(Dot(n, v), 1e-4f);
  V3 h = Norm(Add(l, v));
  f32 ndh = Sat(Dot(n, h));
  f32 vdh = Sat(Dot(v, h));
  f32 a = std::max(roughness * roughness, 1e-5f);
  f32 lobe = D_GGX(ndh, a) * V_Smith(ndv, ndl, a);
  f32 fpow = std::pow(Sat(1.0f - vdh), 5.0f);
  for (int c = 0; c < 3; ++c) {
    out.diffuse[c] = base_color[c] * (1.0f / kPi) * ndl;
    f32 f = f0[c] + (1.0f - f0[c]) * fpow;
    out.specular[c] = lobe * f * ndl;
  }
  return out;
}

}  // namespace rx::render
