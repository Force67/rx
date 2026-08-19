#ifndef RX_HUMAN_EYE_HLSLI_
#define RX_HUMAN_EYE_HLSLI_

// The eye as a layered anatomical system on ONE mesh.
//
// The order the Callisto work insists on - geometry, depth, parallax,
// occlusion, roughness FIRST, exotic spectral effects never - is the order
// implemented here. There is no thin-film simulation in this file on purpose.
//
// The eye mesh is a sphere with a corneal bulge. What this header adds:
//   * the corneal surface refracts the view ray, and the iris is sampled at
//     `iris_depth` BEHIND that surface (parallax + refraction, not a decal);
//   * the pupil dilates by a radial remap that leaves the limbus fixed, so
//     dilation cannot slide the iris edge;
//   * the limbal ring darkens the iris/sclera boundary;
//   * the iris is shadowed separately from the corneal surface, because the
//     limbus occludes oblique light before it ever reaches the pigment;
//   * a weak internal (posterior-surface / lens) reflection proxy.
//
// The corneal REFLECTION is evaluated on the unperturbed corneal normal, which
// is what keeps a catchlight from swimming while the eye rotates: only the
// iris lookup moves, never the specular normal.

#ifndef RX_HUMAN_EYE_PI
#define RX_HUMAN_EYE_PI 3.14159265358979323846
#endif

struct HumanEyeParams {
  float2 iris_center;   // uv of the optical axis on the eye's uv layout
  float iris_radius;    // uv radius of the iris disc
  float iris_depth;     // metres behind the corneal surface
  float pupil_scale;    // >1 dilated, <1 constricted
  float limbal_size;    // uv width of the darkened ring
  float limbal_power;   // ring hardness
  float cornea_ior;
  float iris_shadow_depth;  // 0 = the cornea never shadows the iris
};

struct HumanEyeSample {
  float2 iris_uv;      // where to sample the iris albedo / normal
  float limbal;        // 1 outside the ring, -> 0 at the limbus
  float iris_mask;     // 1 inside the iris disc, 0 on the sclera
  float iris_shadow;   // direct-light attenuation on the pigment
  float lens_reflect;  // weight of the internal reflection proxy
};

// uv_per_metre: the mesh's local uv density, measured from screen-space
// derivatives so `iris_depth` can stay in metres and survive any uv layout.
float HumanEyeUvPerMetre(float2 uv, float3 world_pos) {
  float2 duv = ddx(uv);
  float3 dp = ddx(world_pos);
  float uv_len = length(duv);
  float p_len = length(dp);
  if (p_len < 1e-8 || uv_len < 1e-10) {
    duv = ddy(uv);
    dp = ddy(world_pos);
    uv_len = length(duv);
    p_len = length(dp);
  }
  return (p_len > 1e-8) ? uv_len / p_len : 0.0;
}

// Refract a direction that is expressed in the surface's tangent frame, where
// the surface normal is +Z. `dir_ts` points AWAY from the surface (toward the
// eye or toward the light). Returns the transmitted direction, pointing INTO
// the material (negative z), or 0 on total internal reflection.
float3 HumanEyeRefractTs(float3 dir_ts, float ior) {
  float eta = 1.0 / max(ior, 1.0);
  float3 i = -dir_ts;  // incident, into the surface
  float cosi = -i.z;   // normal is (0,0,1)
  float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
  if (k < 0.0) return float3(0.0, 0.0, 0.0);
  return eta * i + (eta * cosi - sqrt(k)) * float3(0.0, 0.0, 1.0);
}

// Lateral uv offset a ray picks up travelling `depth` metres below the surface.
float2 HumanEyeParallax(float3 refracted_ts, float depth_m, float uv_per_metre) {
  float down = max(-refracted_ts.z, 1e-3);
  return refracted_ts.xy / down * (depth_m * uv_per_metre);
}

// Radial pupil remap. The exponent leaves rn == 1 (the limbus) fixed, so
// dilation moves pigment without ever moving the iris edge.
float2 HumanEyePupil(float2 uv, float2 center, float radius, float scale) {
  float2 d = uv - center;
  float r = length(d);
  if (r < 1e-6 || radius < 1e-6) return uv;
  float rn = saturate(r / radius);
  float rn2 = pow(rn, max(scale, 1e-2));
  return center + (d / r) * (rn2 * radius);
}

// view_ts / light_ts: unit vectors in the corneal surface's tangent frame,
// pointing away from the surface. Pass light_ts = view_ts when there is no
// light to evaluate (the shadow term then reads 1).
HumanEyeSample HumanEyeResolve(HumanEyeParams p, float2 uv, float3 view_ts, float3 light_ts,
                               float uv_per_metre) {
  HumanEyeSample s;
  s.iris_uv = uv;
  s.limbal = 1.0;
  s.iris_mask = 0.0;
  s.iris_shadow = 1.0;
  s.lens_reflect = 0.0;

  float3 refr_v = HumanEyeRefractTs(view_ts, p.cornea_ior);
  float2 offset = (refr_v.z < 0.0)
                      ? HumanEyeParallax(refr_v, p.iris_depth, uv_per_metre)
                      : float2(0.0, 0.0);
  // The ray travels DOWN into the eye, so the pigment we see sits opposite the
  // lateral drift.
  float2 iris_uv = uv + offset;
  iris_uv = HumanEyePupil(iris_uv, p.iris_center, p.iris_radius, p.pupil_scale);
  s.iris_uv = iris_uv;

  float r = length(iris_uv - p.iris_center);
  s.iris_mask = 1.0 - smoothstep(p.iris_radius - max(p.limbal_size, 1e-4) * 0.5,
                                 p.iris_radius, r);

  // Limbal ring: a darkened annulus just inside the limbus. The power shapes
  // how abruptly it bites.
  float ring_start = max(p.iris_radius - max(p.limbal_size, 1e-5), 0.0);
  float ring = saturate((r - ring_start) / max(p.limbal_size, 1e-5));
  s.limbal = 1.0 - pow(ring, max(p.limbal_power, 1e-2));

  // Iris shadow depth, evaluated separately from the corneal surface shadow:
  // trace the LIGHT back out through the cornea and ask where it entered. Light
  // entering outside the limbus has crossed the sclera and never reaches the
  // pigment cleanly.
  if (p.iris_shadow_depth > 0.0) {
    float3 refr_l = HumanEyeRefractTs(light_ts, p.cornea_ior);
    if (refr_l.z < 0.0) {
      float2 entry = iris_uv - HumanEyeParallax(refr_l, p.iris_depth, uv_per_metre);
      float d = length(entry - p.iris_center) / max(p.iris_radius, 1e-5);
      s.iris_shadow = 1.0 - saturate(p.iris_shadow_depth) * saturate(d - 1.0);
    } else {
      s.iris_shadow = 1.0 - saturate(p.iris_shadow_depth);
    }
  }

  // Internal reflection proxy: the posterior corneal surface and the lens send
  // a weak, wide second catchlight back out. It rides the same lobe direction
  // as the corneal reflection but is attenuated by the two extra crossings, so
  // a constant fraction is enough - and it must NOT be view-parallaxed, or it
  // swims.
  s.lens_reflect = 0.06 * s.iris_mask;
  return s;
}

#endif  // RX_HUMAN_EYE_HLSLI_
