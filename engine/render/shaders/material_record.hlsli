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
};

// Skin flag bit (matches MaterialSystem::kFlagSkin and mesh.ps MaterialParams).
static const uint RX_MATERIAL_FLAG_SKIN = 1u << 6;

#endif  // RX_MATERIAL_RECORD_HLSLI_
