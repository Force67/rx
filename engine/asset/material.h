#ifndef RX_ASSET_MATERIAL_H_
#define RX_ASSET_MATERIAL_H_

#include "asset/asset_id.h"
#include "core/types.h"

namespace rx::asset {

enum class AlphaMode : u8 { kOpaque, kMask, kBlend };

// PBR metallic roughness. Legacy spec/gloss materials (e.g. from Bethesda
// shader sets) are approximated into this during conversion.
struct Material {
  AssetId id;
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

}  // namespace rx::asset

#endif  // RX_ASSET_MATERIAL_H_
