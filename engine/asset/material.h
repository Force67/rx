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
  // Subsurface scattering: wrap + back-scatter translucency for skin/wax/leaves.
  f32 subsurface_color[3] = {0.9f, 0.3f, 0.2f};
  f32 subsurface = 0.0f;  // 0 = off
  // Per-texel specular masking, the way the Bethesda lighting shader authors it:
  // the normal map's alpha channel is a specular mask (matte leather and
  // polished steel share one texture) and the material carries the highlight's
  // colour and strength instead of a roughness map. glTF normal maps have no
  // meaningful alpha, so the mask is opt-in; the colour/strength defaults are
  // neutral, so a material that sets neither shades exactly as before.
  bool specular_mask_in_normal_alpha = false;
  f32 specular_color[3] = {1, 1, 1};
  f32 specular_strength = 1.0f;  // 0 = matte (no direct specular lobe)
  // Environment reflection layer over the base material: armour, ice, gems and
  // eyes in the Bethesda games get their shine from a cubemap the material
  // scales and masks, not from being metal. The engine reflects its own
  // environment, so the material only carries the strength (the property's
  // Environment Map Scale) and the mask; the reflection is fresnel weighted,
  // reaching a mirror at grazing angles. 0 = off.
  f32 env_reflect = 0.0f;
  AssetId env_mask;  // r scales env_reflect; falls back to the specular mask
  // Wrap-around light fills, the vanilla "lighting effect" terms. Soft lighting
  // spills the key light past the terminator (leaves, cloth, skin), rim lighting
  // rides the edge of a backlit surface (the value is its falloff exponent), and
  // back lighting transmits straight through. All tint by subsurface_color and
  // are added to the light response, so they show where N.L is 0. 0 = off.
  f32 soft_lighting = 0.0f;
  f32 rim_lighting = 0.0f;
  f32 back_lighting = 0.0f;
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
  // --- Character ("human") surface model -----------------------------------
  // The Callisto-Protocol-style controllable BRDF for skin, lips, teeth, gums
  // and eyes. Enabling it routes the material through one evaluator shared by
  // every direct light type and every render path (raster, hybrid RT, path
  // trace), which is the point: a face must not shade differently under a sun,
  // a spot and a light panel.
  //
  // Every control below is NEUTRAL at its default, and the neutral set
  // reproduces the engine's stock Lambert + GGX exactly - so turning `human`
  // on changes nothing until an artist dials a knob against reference. Fit
  // them with `--demo lookdev` (OLAT rig + split/difference comparison); do
  // not copy constants out of a paper or a slide deck.
  // See engine/render/shaders/human_brdf.hlsli for the shapes.
  enum class HumanRegion : u8 {
    kSkin,      // the default: dermis over subcutaneous scattering
    kLips,      // wetter, thinner, redder transport
    kTeeth,     // enamel over dentin; strong short-range diffusion
    kGums,      // soft, high perfusion
    kSclera,    // the white of the eye; wet, shallow scattering
    kCornea,    // the transparent shell; refracts the iris behind it
    kIris,      // the pigmented disc, shaded behind the cornea
    kTearline,  // the wet meniscus at the lid contact
  };

  struct HumanParams {
    HumanRegion region = HumanRegion::kSkin;

    // Diffuse Fresnel: grazing gain on the diffuse lobe (the boundary
    // transmission loss entering and leaving). `falloff` shapes the view half,
    // `tangent_falloff` the light half; equal values keep the lobe reciprocal.
    f32 diffuse_fresnel_peak = 0.0f;
    f32 diffuse_fresnel_falloff = 5.0f;
    f32 diffuse_fresnel_tangent_falloff = 5.0f;

    // Grazing retroreflection: back-scatter toward the light, the velvety lift
    // skin shows with the key behind the camera. Burley's shape, artist-keyed.
    f32 retroreflection_peak = 0.0f;
    f32 retroreflection_falloff = 5.0f;
    f32 retroreflection_tangent_falloff = 5.0f;

    // Smooth shading terminator: how far past the geometric terminator light
    // wraps (in cosine units) and how strongly. Energy-normalized, so softening
    // the terminator cannot brighten the face overall.
    f32 smooth_terminator_amount = 0.0f;
    f32 smooth_terminator_length = 0.0f;

    // Generalized specular Fresnel exponent. 5 = classic Schlick.
    f32 specular_fresnel_falloff = 5.0f;

    // Optional second GGX lobe: a broad tail under the tight core, blended (not
    // added) so specular energy is unchanged. weight 0 = single lobe.
    f32 secondary_roughness_scale = 3.0f;
    f32 secondary_specular_weight = 0.0f;

    // How much of a light's SHAPE the specular lobe absorbs. 1 = a light with
    // real solid angle cannot produce a highlight tighter than its own image,
    // which is what makes a small hard emitter and a large soft one read as the
    // same material. 0 (the default) is punctual, matching the engine's stock
    // path - so a material that enables `human` and touches nothing shades
    // exactly as it did before.
    f32 light_shape_response = 0.0f;

    // Transport. mean_free_path is in METRES (skin red channel is ~1 mm).
    f32 mean_free_path = 0.001f;
    f32 subsurface_scale = 1.0f;
    // Through-the-surface lobe (ears, nostrils, eyelids, fingers). 0 = opaque.
    f32 transmission = 0.0f;
    f32 transmission_tint[3] = {1.0f, 0.35f, 0.2f};
    f32 extinction_scale = 1.0f;
    // Thickness at thickness_map == 1, in metres. Without a map the shader uses
    // this directly, so a uniform-thickness part still transmits sanely.
    f32 thickness_scale = 0.01f;
    AssetId thickness_map;  // r = normalized local thickness

    // A sharp wet lobe over the base (tear film, saliva, sweat sheen) that dims
    // what is underneath by its own reflectance, so it adds no free energy.
    f32 corneal_wetness = 0.0f;

    // Mouth-cavity occlusion: darkens the INDIRECT term only, which is what a
    // mouth interior actually loses. 0 = no cavity darkening.
    f32 cavity_occlusion = 0.0f;

    // Separate specular normal (Ns). A sweat/tear-film normal map bends the
    // highlight without touching the diffuse lobe - without this split, sweat
    // droplets make skin read as scarred geometry.
    AssetId specular_normal;
    f32 specular_normal_strength = 1.0f;

    // --- eye anatomy (region kCornea / kIris / kSclera) ---------------------
    // The eye is shaded as a layered system on one mesh: the corneal surface
    // refracts the view ray, the iris is sampled at `iris_depth` BEHIND it, and
    // the limbal ring darkens the sclera/iris boundary.
    f32 iris_depth = 0.0028f;      // metres behind the corneal surface
    f32 iris_radius = 0.16f;       // uv radius of the iris disc about the eye centre
    f32 pupil_scale = 1.0f;        // dilation; scales the iris uv about its centre
    f32 limbal_ring_size = 0.035f; // uv width of the ring
    f32 limbal_ring_power = 2.0f;  // ring hardness
    f32 cornea_ior = 1.376f;       // refraction at the corneal surface
    f32 iris_shadow_depth = 0.5f;  // how much the cornea shadows the iris (0 = none)

    // --- Realis-style measured residual ------------------------------------
    // photograph - analytical render, fitted offline into a directional basis
    // and stored as two maps (see tools/fit_residual.py). 0 = analytic only.
    // The runtime fades it out when the material state no longer matches the
    // capture state (blood, dirt, wetness), so a corrected face cannot keep a
    // correction that was measured on a clean one.
    f32 residual_weight = 0.0f;
    AssetId residual_ambient;      // rgb = view-independent residual, a = validity
    AssetId residual_directional;  // rgb = the fitted directional vector (tangent space)
  };
  // Off by default: `human` false leaves every material on the stock path.
  bool human = false;
  HumanParams human_params;

  // Hair cards. `hair` routes the material through the fibre BSDF
  // (render/shaders/hair_bsdf.hlsli) - Marschner's R/TT/TRT lobes with Zinke
  // dual scattering - along the vertex tangent, which for a hair card is the
  // strand direction. The same evaluator the strand grooms use: a character
  // whose card hair and whose simulated strands shade differently has two hair
  // materials, and only one of them can be the right one.
  bool hair = false;
  struct HairParams {
    // Absorption, or the colour to invert into it. See HairColorMode in
    // render/pipeline/hair_material.h: authored colour is the default because
    // hair textures are painted, not measured, and the inversion keeps the
    // colour coupled to how the fibre scatters.
    bool color_from_albedo = true;
    f32 sigma_a[3] = {0.06f, 0.10f, 0.20f};  // only read when color_from_albedo is false
    f32 beta_m = 0.3f;   // longitudinal roughness (highlight width along the strand)
    f32 beta_n = 0.3f;   // azimuthal roughness (highlight width around it)
    f32 alpha = 0.0349066f;  // cuticle scale tilt, radians; separates the two highlights
    f32 eta = 1.55f;         // keratin
    f32 scatter_scale = 1.0f;  // gain on multiple scattering
    // Fibre depth at which the authored colour renders exactly.
    f32 color_reference_depth = 6.0f;
    // A card is a slab standing in for many fibres, and unlike a strand groom
    // it is not in the transmittance volume, so it has no measured depth. This
    // is what it assumes instead. Zero would mean "one isolated fibre", which
    // is the one answer that is definitely wrong for a card.
    f32 assumed_depth = 5.0f;
  };
  HairParams hair_params;
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
