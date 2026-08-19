#ifndef RX_HUMAN_BRDF_HLSLI_
#define RX_HUMAN_BRDF_HLSLI_

// The character ("human") surface model: one controllable BRDF that every
// light type, every render path and every quality tier evaluates, so a face
// cannot shade differently under the sun than under a spot, a panel or a
// traced bounce. This is the transferable half of The Callisto Protocol's
// character work - not one equation, but "every light that touches the
// character runs the same material".
//
// Design contract (do not break it):
//   * The NEUTRAL parameter set (HumanNeutralParams) reproduces the engine's
//     stock Lambert + GGX response bit-for-bit. Everything below is an
//     opt-in deviation an artist dials in against calibrated reference.
//   * Every control is independent. Diffuse Fresnel does not secretly move
//     retroreflection; the terminator does not move energy into the tail.
//   * Diffuse and specular read SEPARATE shading normals (Nd / Ns) so a
//     layered effect - sweat over skin - can bend the highlight without
//     turning the diffuse into scarred geometry.
//
// Source caveat: the published Callisto slides give indicative shapes, not
// production constants. The forms here are the engine's own fits, chosen so
// each knob is monotonic, energy-sane and neutral at zero. Fit them against
// your own OLAT reference (see --demo lookdev); do not treat the defaults as
// measured truth.
//
// References:
//   Burley 2012, "Physically Based Shading at Disney" (retroreflection shape).
//   Heitz 2014 (Smith height-correlated GGX visibility).
//   Hill 2010, "Wrap shading" (energy-normalized terminator softening).
//   Kelemen & Szirmay-Kalos 2001 (separable diffuse/specular for skin).

#ifndef RX_HUMAN_PI
#define RX_HUMAN_PI 3.14159265358979323846
#endif

// --- normalized light input -------------------------------------------------
// Every direct light path fills one of these and hands it to the same
// evaluator. `direction` points FROM the surface TOWARD the light. radiance is
// scene-linear and already carries the light's intensity and distance
// attenuation; solid_angle drives the light-shape-aware roughness widening.
struct HumanLightSample {
  float3 direction;
  float3 radiance;
  float distance;
  float solid_angle;             // steradians subtended at the shading point
  float visibility;              // shadow / occlusion on the reflected lobes
  float transmission_visibility; // shadow on the through-the-surface lobe
  uint type;                     // RX_HUMAN_LIGHT_*
  uint flags;                    // RX_HUMAN_LIGHT_FLAG_*
};

static const uint RX_HUMAN_LIGHT_DIRECTIONAL = 0u;
static const uint RX_HUMAN_LIGHT_POINT = 1u;
static const uint RX_HUMAN_LIGHT_SPOT = 2u;
static const uint RX_HUMAN_LIGHT_SPHERE = 3u;
static const uint RX_HUMAN_LIGHT_RECT = 4u;
static const uint RX_HUMAN_LIGHT_AMBIENT = 5u;   // pre-integrated, no direction

// The caller already integrated the light's shape (LTC / area form factor) and
// wants only the material's directional modifiers, not a second cosine.
static const uint RX_HUMAN_LIGHT_FLAG_PREINTEGRATED = 1u;

// --- shading normals --------------------------------------------------------
// geometric  : the interpolated vertex normal, before any map. Terminator and
//              shadow-bias decisions use it, because a normal map must not be
//              able to push a surface past its own geometric horizon.
// diffuse    : Nd. Skin, with its detail normals filtered for the diffuse lobe.
// specular   : Ns. The layer that owns the highlight (sweat, tear film, oil).
struct HumanShadingNormals {
  float3 geometric;
  float3 diffuse;
  float3 specular;
};

HumanShadingNormals HumanNormals(float3 n) {
  HumanShadingNormals s;
  s.geometric = n;
  s.diffuse = n;
  s.specular = n;
  return s;
}

// --- surface parameters -----------------------------------------------------
// Mirrors render::HumanSurfaceParameters (pipeline/human_material.h) and the
// tail of MaterialParams in mesh.ps.hlsl. Keep the three in sync.
struct HumanSurfaceParams {
  float3 base_color;
  float roughness;               // primary lobe, perceptual (a = roughness^2)
  float3 specular_f0;
  float metallic;                // human surfaces are dielectric; kept for parity

  // Diffuse Fresnel: the grazing gain of the diffuse lobe. peak 0 = off.
  // `falloff` shapes the VIEW half, `tangent_falloff` the LIGHT half; equal
  // values keep the lobe reciprocal.
  float diffuse_fresnel_peak;
  float diffuse_fresnel_falloff;
  float diffuse_fresnel_tangent_falloff;

  // Grazing retroreflection: back-scatter toward the light (the dusty, velvety
  // lift skin shows when the key sits behind the camera). peak 0 = off.
  float retro_peak;
  float retro_falloff;
  float retro_tangent_falloff;

  // Smooth shading terminator: how far past the geometric terminator light
  // wraps, and how strongly. amount 0 = hard Lambert terminator.
  float smooth_terminator_amount;
  float smooth_terminator_length;

  // Generalized specular Fresnel. 5 = classic Schlick.
  float spec_fresnel_falloff;

  // Optional second GGX lobe: a broad tail under the tight core. weight 0 =
  // single lobe (and the scale is then irrelevant).
  float secondary_roughness_scale;
  float secondary_spec_weight;

  // How much of a light's SHAPE the lobe absorbs. 1 = a light with real solid
  // angle cannot produce a highlight tighter than its own image, which is what
  // makes a small hard emitter and a large soft one read as the same material.
  // 0 = punctual, which is what the engine's stock path assumes - and is
  // therefore what the neutral set has to use for neutral to mean neutral.
  float light_shape_response;

  // Transport. mean_free_path is metres; subsurface_scale multiplies it.
  float mean_free_path;
  float subsurface_scale;
  float transmission;            // through-the-surface lobe, 0 = opaque
  float3 transmission_tint;
  float extinction_scale;        // thickness -> optical depth

  // Layer / anatomy controls.
  float corneal_wetness;         // sharp wet lobe over the base (eyes, lips, saliva)
  float residual_weight;         // Realis-style measured correction, 0 = analytic only
  float cavity_occlusion;        // mouth-interior darkening applied to indirect
};

// The neutral set: this reproduces Lambert + GGX. Any renderer change that
// makes HumanNeutralParams differ from the stock path is a regression - the
// lookdev demo asserts it (--demo lookdev, "neutral parity" readout) and
// human_brdf_test.cc checks it on the CPU mirror.
HumanSurfaceParams HumanNeutralParams(float3 base_color, float roughness, float3 f0) {
  HumanSurfaceParams p;
  p.base_color = base_color;
  p.roughness = roughness;
  p.specular_f0 = f0;
  p.metallic = 0.0;
  p.diffuse_fresnel_peak = 0.0;
  p.diffuse_fresnel_falloff = 5.0;
  p.diffuse_fresnel_tangent_falloff = 5.0;
  p.retro_peak = 0.0;
  p.retro_falloff = 5.0;
  p.retro_tangent_falloff = 5.0;
  p.smooth_terminator_amount = 0.0;
  p.smooth_terminator_length = 0.0;
  p.spec_fresnel_falloff = 5.0;
  p.secondary_roughness_scale = 3.0;
  p.secondary_spec_weight = 0.0;
  p.light_shape_response = 0.0;
  p.mean_free_path = 0.0;
  p.subsurface_scale = 1.0;
  p.transmission = 0.0;
  p.transmission_tint = float3(1.0, 1.0, 1.0);
  p.extinction_scale = 1.0;
  p.corneal_wetness = 0.0;
  p.residual_weight = 0.0;
  p.cavity_occlusion = 0.0;
  return p;
}

// --- lobes ------------------------------------------------------------------
float HumanD_GGX(float ndh, float a) {
  float a2 = a * a;
  float d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / max(RX_HUMAN_PI * d * d, 1e-7);
}

float HumanV_SmithCorrelated(float ndv, float ndl, float a) {
  float a2 = a * a;
  float gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
  float gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-5);
}

// Generalized Schlick. falloff == 5 is the classic form, so the neutral set
// leaves the stock renderer's fresnel untouched.
float3 HumanFresnel(float3 f0, float u, float falloff) {
  return f0 + (1.0 - f0) * pow(saturate(1.0 - u), max(falloff, 1e-2));
}

// Energy-normalized wrapped cosine (Hill). At w == 0 this is saturate(ndl);
// as w grows, light reaches `w` past the geometric terminator and the
// 1/(1+w)^2 factor keeps the hemispherical integral at unity, so softening the
// terminator cannot brighten the face overall.
float HumanWrapCosine(float ndl, float w) {
  float ww = max(w, 0.0);
  return saturate((ndl + ww) / ((1.0 + ww) * (1.0 + ww)));
}

// The cosine the DIFFUSE lobe integrates against. Specular keeps the hard
// cosine: a soft terminator is a subsurface transport effect, and letting it
// widen the highlight is exactly the "looks like wax" failure.
float HumanDiffuseCosine(HumanSurfaceParams p, float ndl_shading, float ndl_geom) {
  float hard = saturate(ndl_shading);
  if (p.smooth_terminator_amount <= 0.0) return hard;
  float soft = HumanWrapCosine(ndl_shading, p.smooth_terminator_length);
  // The geometric normal gates the wrap: a normal map may soften the
  // terminator, never carry light around the far side of the head.
  float gate = saturate(ndl_geom * 4.0 + 1.0);
  return lerp(hard, soft, saturate(p.smooth_terminator_amount) * gate);
}

// Diffuse Fresnel + retroreflection as two INDEPENDENT multipliers on the
// diffuse lobe. Both are 1.0 at their neutral peaks, so a neutral material
// gets exactly base_color/pi.
//
// Diffuse Fresnel is the transmission loss at the air/skin boundary, entered
// and exited; it darkens (peak < 0) or lifts (peak > 0) the grazing diffuse.
// Retroreflection follows Burley's shape - keyed on the half-vector/light dot
// so it peaks when the eye looks down the light - with the fixed pow5 and the
// roughness coupling replaced by artist controls.
float HumanDiffuseShaping(HumanSurfaceParams p, float ndl, float ndv, float ldh) {
  float grazing_v = pow(saturate(1.0 - ndv), max(p.diffuse_fresnel_falloff, 1e-2));
  float grazing_l = pow(saturate(1.0 - ndl), max(p.diffuse_fresnel_tangent_falloff, 1e-2));
  float fresnel = (1.0 + p.diffuse_fresnel_peak * grazing_v) *
                  (1.0 + p.diffuse_fresnel_peak * grazing_l);

  float fd90 = p.retro_peak * ldh * ldh;
  float retro_v = 1.0 + fd90 * pow(saturate(1.0 - ndv), max(p.retro_tangent_falloff, 1e-2));
  float retro_l = 1.0 + fd90 * pow(saturate(1.0 - ndl), max(p.retro_falloff, 1e-2));

  return max(fresnel * retro_v * retro_l, 0.0);
}

// Light-shape-aware roughness: a light with real solid angle cannot produce a
// highlight tighter than its own image. Widening the lobe here - once, in the
// shared evaluator - is what keeps a small hard emitter and a large soft panel
// reading as the SAME material instead of two different ones.
float HumanShapeRoughness(float roughness, float solid_angle, float response) {
  solid_angle *= saturate(response);
  if (solid_angle <= 0.0) return roughness;
  // Half-angle of the equivalent cone; converted to a roughness the lobe can
  // absorb, then combined in variance (a^2) space.
  float half_angle = sqrt(max(solid_angle, 0.0) / RX_HUMAN_PI);
  float widen = saturate(half_angle * 0.5);
  return saturate(sqrt(roughness * roughness + widen * widen));
}

// --- the evaluator ----------------------------------------------------------
// Returns cosine-weighted lobes: multiply by the light's radiance and add.
// Keeping the cosine INSIDE is what lets the terminator control live here
// instead of being re-derived (differently) at every call site.
struct HumanBrdfResult {
  float3 diffuse;
  float3 specular;
  float3 transmission;
};

HumanBrdfResult HumanBrdfZero() {
  HumanBrdfResult r;
  r.diffuse = 0.0.xxx;
  r.specular = 0.0.xxx;
  r.transmission = 0.0.xxx;
  return r;
}

// thickness: metres of material between the shading point and the far side, for
// the transmission lobe. Pass 0 when unknown (the lobe then reads as opaque).
HumanBrdfResult HumanEvaluate(HumanSurfaceParams p, HumanShadingNormals n, float3 v,
                              HumanLightSample light, float thickness) {
  HumanBrdfResult r = HumanBrdfZero();
  float3 l = light.direction;

  float ndl_d = dot(n.diffuse, l);
  float ndv_d = max(dot(n.diffuse, v), 1e-4);
  float ndl_g = dot(n.geometric, l);

  // ---- diffuse -------------------------------------------------------------
  float cos_d = HumanDiffuseCosine(p, ndl_d, ndl_g);
  if (cos_d > 0.0) {
    float3 hd = normalize(l + v);
    float ldh = saturate(dot(l, hd));
    float shaping = HumanDiffuseShaping(p, saturate(ndl_d), ndv_d, ldh);
    float3 albedo = p.base_color * (1.0 - p.metallic);
    r.diffuse = albedo * (1.0 / RX_HUMAN_PI) * shaping * cos_d;
  }

  // ---- specular ------------------------------------------------------------
  float ndl_s = dot(n.specular, l);
  if (ndl_s > 0.0) {
    float ndl = saturate(ndl_s);
    float ndv = max(dot(n.specular, v), 1e-4);
    float3 h = normalize(l + v);
    float ndh = saturate(dot(n.specular, h));
    float vdh = saturate(dot(v, h));

    float rough = HumanShapeRoughness(p.roughness, light.solid_angle, p.light_shape_response);
    float a1 = max(rough * rough, 1e-5);
    float3 f = HumanFresnel(p.specular_f0, vdh, p.spec_fresnel_falloff);

    float core = HumanD_GGX(ndh, a1) * HumanV_SmithCorrelated(ndv, ndl, a1);
    float lobe = core;
    if (p.secondary_spec_weight > 0.0) {
      // A single GGX cannot hold a tight core AND the broad tail a real
      // dermis/oil stack throws. The second lobe is a weighted blend, not an
      // addition, so total specular energy is unchanged.
      float rough2 = saturate(rough * max(p.secondary_roughness_scale, 1e-2));
      float a2 = max(rough2 * rough2, 1e-5);
      float tail = HumanD_GGX(ndh, a2) * HumanV_SmithCorrelated(ndv, ndl, a2);
      lobe = lerp(core, tail, saturate(p.secondary_spec_weight));
    }
    r.specular = lobe * f * ndl;

    // Wet layer (tear film, saliva, sweat sheen): a near-mirror lobe over the
    // base, dimming what is underneath by its own reflectance so it adds no
    // free energy.
    if (p.corneal_wetness > 0.0) {
      float wa = max(0.02 * 0.02, 1e-5);
      float wf = (0.02 + 0.98 * pow(saturate(1.0 - vdh), 5.0)) * saturate(p.corneal_wetness);
      float wet = HumanD_GGX(ndh, wa) * HumanV_SmithCorrelated(ndv, ndl, wa) * ndl;
      r.specular = r.specular * (1.0 - wf) + wet * wf;
      r.diffuse *= (1.0 - wf);
    }
  }

  // ---- transmission --------------------------------------------------------
  // Light entering the far side and leaving toward the eye: ears, nostrils,
  // eyelids, fingers. Beer-Lambert over the local thickness, view-aligned so
  // it only shows where you are looking into the light.
  if (p.transmission > 0.0 && ndl_d < 0.35) {
    float3 optical = max(p.extinction_scale * thickness / max(p.mean_free_path * p.subsurface_scale, 1e-4), 0.0).xxx;
    float3 attenuation = exp(-optical);
    float back = saturate(dot(v, -l));
    // A wide, view-aligned lobe; the exponent keeps it from wrapping onto the
    // lit side where the diffuse already accounts for the energy.
    float lobe = pow(back, 2.0) * saturate(0.35 - ndl_d) / 0.35;
    r.transmission = p.base_color * p.transmission_tint * p.transmission * attenuation *
                     lobe * light.transmission_visibility;
  }

  float vis = light.visibility;
  r.diffuse *= vis;
  r.specular *= vis;
  return r;
}

// Pre-integrated variant for lights whose SHAPE the caller already integrated
// (LTC panels and spheres). `diffuse_integral` / `specular_integral` are the
// form factors; the material's directional modifiers are evaluated once at the
// representative direction so a panel and a point light of equal power land on
// the same material semantics. Approximate by construction - documented here so
// nobody "fixes" it into a second material model.
HumanBrdfResult HumanEvaluatePreintegrated(HumanSurfaceParams p, HumanShadingNormals n,
                                           float3 v, float3 rep_dir, float diffuse_integral,
                                           float3 specular_integral, float visibility) {
  HumanBrdfResult r = HumanBrdfZero();
  float ndl = saturate(dot(n.diffuse, rep_dir));
  float ndv = max(dot(n.diffuse, v), 1e-4);
  float3 h = normalize(rep_dir + v);
  float ldh = saturate(dot(rep_dir, h));
  float shaping = HumanDiffuseShaping(p, ndl, ndv, ldh);
  // The terminator softening rides the ratio, not the integral: LTC already
  // carries the cosine.
  float cos_hard = max(ndl, 1e-4);
  float cos_soft = HumanDiffuseCosine(p, dot(n.diffuse, rep_dir), dot(n.geometric, rep_dir));
  float terminator = cos_soft / cos_hard;

  r.diffuse = p.base_color * (1.0 - p.metallic) * diffuse_integral * shaping * terminator * visibility;
  r.specular = specular_integral * visibility;
  return r;
}

#endif  // RX_HUMAN_BRDF_HLSLI_
