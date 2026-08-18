#ifndef RX_ASSET_MATERIAL_H_
#define RX_ASSET_MATERIAL_H_

#include <string>

#include "asset/asset_id.h"
#include "core/types.h"

namespace rx::asset {

enum class AlphaMode : u8 { kOpaque, kMask, kBlend };

// PBR metallic roughness. Legacy spec/gloss materials (e.g. from Bethesda
// shader sets) are approximated into this during conversion.
struct Material {
  AssetId id;
  // Source material name, verbatim, when the format carries one (glTF
  // `materials[].name`). rx itself only reads it for the wind/water name
  // heuristics below, but a game routing submeshes to its own shading model
  // has nothing else to key on: the AssetId is a hash and does not invert.
  // Empty when the source is unnamed.
  std::string name;
  AssetId base_color;
  AssetId normal;
  // Model-space normal map (_msn): the map stores the surface normal in the
  // mesh's object space (e.g. Bethesda head/face maps), not a tangent-space delta.
  // The shader rotates it straight to world by the model matrix instead of
  // building a TBN; sampling an _msn map as tangent-space smears the lighting.
  bool normal_model_space = false;
  // glTF ORM packing: g = roughness, b = metallic. Engines that ship separate
  // metallic / occlusion maps (e.g. Bethesda Starfield: slot 3 roughness, slot 4
  // metallic, slot 5 AO) point `metallic_roughness` at the roughness map and set
  // `separate_metallic` so the shader reads metallic from `metallic_map.r`
  // instead of the combined `.b`. Both default empty so combined-ORM and
  // untextured materials shade exactly as before.
  AssetId metallic_roughness;
  AssetId metallic_map;   // dedicated metallic (r) when separate_metallic is set
  AssetId occlusion_map;  // dedicated ambient-occlusion (r), multiplies indirect
  bool separate_metallic = false;
  AssetId emissive;
  f32 base_color_factor[4] = {1, 1, 1, 1};
  f32 metallic_factor = 0;
  f32 roughness_factor = 1;
  // Ambient-occlusion strength: lerps the sampled occlusion toward 1 (no
  // effect). Only applied when an occlusion_map is bound; 1 = full AO.
  f32 ao_strength = 1.0f;
  f32 emissive_factor[3] = {0, 0, 0};
  f32 alpha_cutoff = 0.5f;
  // Extended pbr lobes (glTF KHR_materials_*). Defaults are neutral/off so a
  // plain metallic-roughness material is unchanged.
  f32 clearcoat = 0.0f;            // KHR_materials_clearcoat
  f32 clearcoat_roughness = 0.0f;
  f32 anisotropy = 0.0f;           // KHR_materials_anisotropy, -1..1
  f32 ior = 1.5f;                  // KHR_materials_ior, dielectric f0
  f32 sheen_color[3] = {0, 0, 0};  // KHR_materials_sheen
  f32 sheen_roughness = 0.3f;
  // OpenPBR Surface (AcademySoftwareFoundation/OpenPBR v1.1.1) additions, for
  // the lobes glTF has no equivalent of. Every default here is the *glTF*
  // default, not the OpenPBR one, so an imported metallic-roughness material
  // shades exactly as it did before this block existed. The OpenPBR importers
  // (usd_loader, materialx) apply the spec defaults themselves for inputs a
  // document leaves unauthored - they differ, and silently taking the glTF
  // value for an OpenPBR asset would be wrong. See docs/OPENPBR.md.
  //
  // Roughness of the Oren-Nayar diffuse lobe. 0 = Lambert (the glTF model), and
  // the shader keeps the cheap Lambert path at 0 rather than evaluating EON.
  f32 base_diffuse_roughness = 0.0f;  // openpbr base_diffuse_roughness
  // Modulates dielectric reflectivity at normal incidence by reducing the ior
  // below specular_ior; also scales the metal Fresnel. May exceed 1 (the shader
  // clamps against the physical 1/F0 ceiling). 1 = the ior alone decides.
  f32 specular_weight = 1.0f;  // openpbr specular_weight
  // Dielectric: tints the Fresnel of the primary specular reflection only.
  // Metal: the reflectivity at the ~82 degree grazing edge, as a fraction of
  // the Schlick curve, feeding the F82-tint model (Kutz 2021). White reduces
  // F82 to plain Schlick, which is what every non-OpenPBR material wants.
  f32 specular_color[3] = {1, 1, 1};  // openpbr specular_color
  // Square of the coat's normal-incidence transmittance, i.e. the tint the coat
  // absorption applies to the base. White = a clear coat.
  f32 coat_color[3] = {1, 1, 1};  // openpbr coat_color
  // Coat ior. 1.5 is the glTF KHR_materials_clearcoat value (f0 = 0.04) and the
  // engine's historical hardcode; OpenPBR's own default is 1.6.
  f32 coat_ior = 1.5f;  // openpbr coat_ior
  // How much of the physical coat darkening (internal reflections striking the
  // base repeatedly) to apply. OpenPBR defaults this to 1 (fully physical); the
  // engine defaults to 0 so existing clearcoat materials keep their look.
  f32 coat_darkening = 0.0f;  // openpbr coat_darkening
  // Thin-film ior. 1.3 is the KHR_materials_iridescence default and the
  // engine's historical hardcode; OpenPBR's own default is 1.4.
  f32 thin_film_ior = 1.3f;  // openpbr thin_film_ior
  // Subsurface scattering: wrap + back-scatter translucency for skin/wax/leaves.
  f32 subsurface_color[3] = {0.9f, 0.3f, 0.2f};
  f32 subsurface = 0.0f;  // 0 = off
  // Thin-film interference (KHR_materials_iridescence): a view-angle dependent
  // rainbow on the specular, for soap bubbles, oil, beetle shells.
  f32 iridescence = 0.0f;
  f32 iridescence_thickness = 400.0f;  // film thickness in nm
  // Transmission (KHR_materials_transmission): refract the scene behind the
  // surface instead of diffusing, for glass. Routed to the transparent pass.
  f32 transmission = 0.0f;
  AlphaMode alpha_mode = AlphaMode::kOpaque;
  bool two_sided = false;
  // Routed to the dedicated water pipeline: animated waves, raytraced
  // reflections, refraction with absorption. base_color acts as the
  // absorption tint, roughness scales the wave choppiness.
  bool is_water = false;
  // Runtime terrain splat: the four texture slots are reused as three land
  // layers (base_color/normal/metallic_roughness) plus a per-cell weight map
  // (emissive). The shader tiles the layers at the native land repeat and
  // blends them by the weight map instead of the usual base-color sample.
  bool is_terrain = false;
  // Terrain splat v2: a per-cell palette of up to 8 land layers, sampled
  // through the bindless table and blended by two RGBA8 weight maps (emissive
  // = palette slots 0-3, height = slots 4-7; weights renormalize in the
  // shader so bilinear filtering stays valid). Per-layer normal maps come
  // from the LTEX texture sets; a zero id keeps that layer flat.
  // terrain_layer_count 0 keeps the legacy 3-layer path above (also the path
  // the ray-traced hit shading continues to approximate).
  u32 terrain_layer_count = 0;
  AssetId terrain_layers[8];
  AssetId terrain_layer_normals[8];
  // Height/displacement map (r channel, 1 = surface, 0 = deepest) for
  // parallax occlusion mapping; scale is the depth in uv-tangent units.
  AssetId height;
  f32 height_scale = 0.05f;
  // Silhouette-aware POM: approximate the underlying mesh as a locally curved
  // patch so the height march bends over the surface, and discard fragments
  // whose view ray exits the height shell near grazing convex edges - carving
  // the heightfield profile into the object outline instead of leaving the flat
  // polygon silhouette (Crimson Desert-style POM). Only meaningful on curved
  // geometry with a height map. silhouette_curvature scales the per-pixel mesh
  // curvature the shader derives (1 = as-measured; lower softens the carve).
  bool silhouette_pom = false;
  f32 silhouette_curvature = 1.0f;
  // Animated texture scroll from a NIF shader float controller (U/V Offset),
  // in uv units per second. The raster shaders add frame.time * this to the uv
  // before sampling, so waterfalls/rivers/lava flow. 0 = static.
  f32 uv_scroll_u = 0;
  f32 uv_scroll_v = 0;
  // Vertex wind sway (banners, curtains, foliage). Weight convention: uv.y
  // grows away from the attachment (0 = pinned edge).
  bool wind = false;
  // Skin: the scene pass exports this material's diffuse lighting to the
  // subsurface buffer and the screen-space SSS blur diffuses it (red bleed at
  // shadow edges). Independent of `subsurface` (the analytic transmission term).
  bool skin = false;
  // Physically based skin subsurface scattering (unified across the raster,
  // hybrid-RT and path-traced paths; see render/shaders/sss_profile.hlsli).
  // Artist authors a diffuse scatter colour and a per-channel mean free path
  // (mm); Kulla-Conty 2017 maps these to single-scattering albedo and the
  // engine derives sigma_t / sigma_s at upload. Defaults approximate the
  // classic 3-layer caucasian skin dmfp. Only consumed when `skin` is set.
  struct SkinParams {
    // Multiple-scattering (diffuse) colour the artist wants to see.
    f32 scatter_color[3] = {0.85f, 0.55f, 0.40f};
    // Per-channel mean free path in millimetres (red travels furthest).
    f32 mfp[3] = {1.0f, 0.35f, 0.20f};
    // Uniform scale on the mean free path (thicker/thinner skin, tuning).
    f32 scatter_scale = 1.0f;
    // Henyey-Greenstein phase anisotropy (skin is mildly forward, ~0.0-0.3).
    f32 anisotropy_g = 0.0f;
    // Boundary index of refraction (skin ~1.4).
    f32 ior = 1.4f;
    // Baseline hemoglobin perfusion 0..1; the dynamic blood-flow system drives
    // it up (flush) or down (blanch) at runtime. 0.5 = resting.
    f32 perfusion = 0.5f;
  };
  SkinParams skin_params;
  // Hair: dual-lobe Kajiya-Kay strand specular along the vertex tangent
  // (strand direction) replaces the GGX sun response; roughness drives the
  // highlight width. Pair with alpha-masked cards for real hair.
  bool hair = false;
  // Albedo comes from the engine's virtual-texture space instead of the
  // base_color texture (feedback-streamed page atlas; see VirtualTexture).
  bool virtual_albedo = false;
  // BSEffectShaderProperty geometry (torch/campfire flames, glow planes, god
  // rays, mist sheets, shrine glows): shaded unlit as source texture *
  // base_color_factor (the emissive colour * multiple) * vertex colour, blended,
  // no lighting/shadows/decals. base_color holds the source texture; emissive
  // holds the optional greyscale-to-palette texture.
  bool effect = false;
  bool effect_additive = false;        // additive (fire) vs alpha (mist) blend
  bool effect_grayscale_color = false; // remap source luminance through the palette
  bool effect_grayscale_alpha = false; // source/palette alpha comes from luminance
  bool effect_falloff = false;         // view-angle opacity fade (glow planes)
  // start angle, stop angle, start opacity, stop opacity (dot-of-view thresholds).
  f32 effect_falloff_params[4] = {1, 1, 1, 1};
  // Emissive pulse from a shader emissive-multiple controller: x = frequency
  // (Hz), y = amount (0..1 of the mean it swings). 0 = constant.
  f32 emissive_pulse[2] = {0, 0};
};

// OpenPBR specifies anisotropy as a stretch a in [0,1] with the NDF axes
// related by alpha_b/alpha_t = 1 - a, while the engine's shader parametrizes it
// as ax = alpha*(1+k), ay = alpha*(1-k) with k in [-1,1]. Matching the axis
// ratio gives k = a/(2-a). This reproduces the shape of the highlight but not
// the spec's alpha_t^2 + alpha_b^2 = 2*alpha^2 mean-roughness normalization, so
// a strongly anisotropic surface reads a little rougher overall than it would
// in a reference renderer. Shared by the USD and MaterialX importers.
inline f32 OpenPbrAnisotropyToEngine(f32 anisotropy) {
  const f32 a = anisotropy < 0.0f ? 0.0f : (anisotropy > 1.0f ? 1.0f : anisotropy);
  return a > 0.0f ? a / (2.0f - a) : 0.0f;
}

// OpenPBR's own defaults for the inputs where they differ from the engine's
// (which follow glTF). An importer seeds these before parsing so that inputs a
// document leaves unauthored land on the spec value rather than the glTF one.
inline void ApplyOpenPbrDefaults(Material* m) {
  m->base_color_factor[0] = m->base_color_factor[1] = m->base_color_factor[2] = 0.8f;
  m->roughness_factor = 0.3f;   // specular_roughness
  m->coat_ior = 1.6f;
  m->coat_darkening = 1.0f;
  m->thin_film_ior = 1.4f;
  m->iridescence_thickness = 500.0f;  // 0.5um
  m->sheen_roughness = 0.5f;          // fuzz_roughness
  m->subsurface_color[0] = m->subsurface_color[1] = m->subsurface_color[2] = 0.8f;
}

}  // namespace rx::asset

#endif  // RX_ASSET_MATERIAL_H_
