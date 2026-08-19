#ifndef RX_MATERIAL_RECORD_HLSLI_
#define RX_MATERIAL_RECORD_HLSLI_

// Canonical GPU material record for ray hit shading. This is the single source
// of truth mirrored by BindlessRegistry::MaterialRecord (render/core/bindless.h)
// and consumed by every RT shader (path tracer, recon g-buffer, ddgi, reflection
// trace, rcgi cache, water, hybrid mesh). Do NOT redeclare this struct inline in
// shaders - include this header instead, so the layout can never drift.
//
// Layout is std430-compatible: 16-byte rows. The first four rows (64 B) are the
// historical record; changing an existing field's offset breaks all consumers,
// so new data is appended.
//
// The terrain_layer1_texture / terrain_weight_texture slots alias what other
// paths treat as padding (they are only read on the terrain-splat branch).
struct MaterialRecord {
  float4 base_color_factor;         // row 0
  float3 emissive;                  // row 1
  uint base_color_texture;
  uint flags;                       // row 2: bit0 alpha mask, bit1 terrain, bit6 skin
  float alpha_cutoff;
  float roughness;
  float metallic;
  uint metallic_roughness_texture;  // row 3 (terrain: land layer 2)
  uint terrain_layer1_texture;      //        (terrain: land layer 1)
  uint terrain_weight_texture;      //        (terrain: per-cell weight map)
  uint pad2;
  // --- Skin subsurface scattering (only meaningful when flags has the skin
  // bit). Coefficients are per-channel and pre-mapped from artist colour/mfp
  // via Kulla-Conty at upload; the shaders consume physical quantities. See
  // sss_profile.hlsli. sigma_a = sigma_t - sigma_s; albedo = sigma_s / sigma_t.
  float3 sss_sigma_t;               // row 4: extinction, 1/world-unit
  float sss_anisotropy_g;           //        Henyey-Greenstein g
  float3 sss_sigma_s;               // row 5: scattering coefficient
  float sss_perfusion;              //        dynamic hemoglobin concentration 0..1
  float3 sss_scatter_color;         // row 6: multiple-scatter tint (for flush coupling)
  float sss_ior;                    //        boundary index of refraction
  // --- Character surface model (only meaningful under RX_MATERIAL_FLAG_HUMAN).
  // The ray paths carry the shaping controls so a traced face and a rastered
  // face are the same material; see human_brdf.hlsli.
  float4 human_diffuse_fresnel;     // row 7: peak, falloff, tangent falloff, retro peak
  float4 human_retro;               // row 8: retro falloff, retro tangent falloff, term amount, term length
  float4 human_spec;                // row 9: spec fresnel falloff, secondary scale, secondary weight, mfp (m)
  float4 human_transmission;        // row 10: transmission, tint rgb
  float4 human_extra;               // row 11: x light-shape response; y/z/w reserved
};

// Skin flag bit (matches MaterialSystem::kFlagSkin and mesh.ps MaterialParams).
static const uint RX_MATERIAL_FLAG_SKIN = 1u << 6;
// Character surface model (matches MaterialSystem::kFlagHuman).
static const uint RX_MATERIAL_FLAG_HUMAN = 1u << 21;

// Unpacks a hit's character parameters into the shared evaluator's struct.
// Requires human_brdf.hlsli to be included first; every ray path that shades a
// character calls this rather than re-deriving the controls, which is the whole
// point of the record carrying them.
#ifdef RX_HUMAN_BRDF_HLSLI_
HumanSurfaceParams RxHumanFromRecord(MaterialRecord m, float3 albedo, float roughness,
                                     float3 f0) {
  HumanSurfaceParams p = HumanNeutralParams(albedo, roughness, f0);
  if ((m.flags & RX_MATERIAL_FLAG_HUMAN) == 0u) return p;
  p.diffuse_fresnel_peak = m.human_diffuse_fresnel.x;
  p.diffuse_fresnel_falloff = m.human_diffuse_fresnel.y;
  p.diffuse_fresnel_tangent_falloff = m.human_diffuse_fresnel.z;
  p.retro_peak = m.human_diffuse_fresnel.w;
  p.retro_falloff = m.human_retro.x;
  p.retro_tangent_falloff = m.human_retro.y;
  p.smooth_terminator_amount = m.human_retro.z;
  p.smooth_terminator_length = m.human_retro.w;
  p.spec_fresnel_falloff = m.human_spec.x;
  p.secondary_roughness_scale = m.human_spec.y;
  p.secondary_spec_weight = m.human_spec.z;
  p.mean_free_path = m.human_spec.w;
  p.transmission = m.human_transmission.x;
  p.transmission_tint = m.human_transmission.yzw;
  p.light_shape_response = m.human_extra.x;
  return p;
}
#endif

#endif  // RX_MATERIAL_RECORD_HLSLI_
