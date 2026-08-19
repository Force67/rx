#ifndef RX_RENDER_HAIR_MATERIAL_H_
#define RX_RENDER_HAIR_MATERIAL_H_

#include "core/types.h"

namespace rx::render {

// CPU side of the hair BSDF (render/shaders/hair_bsdf.hlsli): the resolved
// parameter block, the pigment mappings, the shipped hair-colour presets, the
// quality tiers, and a faithful CPU mirror of the shader so the energy and
// reciprocity contracts can be tested without a GPU.
//
// Hair colour is authored as PIGMENT, not as albedo. A blonde fibre is not a
// brown fibre with a lighter swatch: it absorbs less, so it also transmits and
// forward-scatters more, and that coupling is most of what makes light hair
// read as hair. Tinting an albedo breaks the coupling and produces the classic
// "dark straw" blonde. HairSigmaFromColor exists for artists who want to author
// a target colour anyway, and it goes through the same absorption path.

// Where a fibre's absorption comes from.
//   kPigment   sigma_a is authored directly (melanin concentrations).
//   kAuthored  the groom's per-strand colour is the TARGET multiple-scattering
//              colour, inverted to absorption through Chiang's fit.
// The second is the default because grooms carry colours sampled from a hair
// texture and artists expect those to mean what they look like. It is not a
// shortcut past the physics: the colour becomes absorption, so a light strand
// still transmits and forward-scatters more than a dark one. Tinting the
// shaded result instead - the obvious alternative - is what breaks the coupling
// and gives you blonde hair that scatters like brown.
enum class HairColorMode : u8 { kPigment, kAuthored };

struct HairSurfaceParameters {
  f32 sigma_a[3] = {0.06f, 0.10f, 0.20f};  // absorption per unit fibre length
  f32 beta_m = 0.3f;      // longitudinal roughness (highlight width along the strand)
  f32 beta_n = 0.3f;      // azimuthal roughness (highlight width around it)
  f32 alpha = 0.0349066f; // cuticle scale tilt, radians; ~2 degrees on human hair
  f32 eta = 1.55f;        // keratin
  f32 density = 1.0f;     // groom fibre density multiplier for dual scattering
  f32 scatter_scale = 1.0f;  // artist gain on multiple scattering
  HairColorMode color_mode = HairColorMode::kAuthored;
  // Fibre depth at which an authored colour renders exactly. Roughly how deep
  // into a groom the "colour of the hair" is judged from.
  f32 color_reference_depth = 6.0f;
};

// Eumelanin (brown/black) and pheomelanin (red/yellow) concentrations to
// absorption. Chiang et al. 2016.
void HairSigmaFromMelanin(f32 eumelanin, f32 pheomelanin, f32 out_sigma[3]);

// Chiang's published colour inversion. Calibrated against FULL path-traced
// multiple scattering, so it does NOT apply to this renderer's dual-scattering
// transport - measured here, asking it for 0.45 renders as 0.77. Kept for a
// path tracer and because the discrepancy is worth being able to reproduce.
void HairSigmaFromColorPathTraced(const f32 color[3], f32 beta_n, f32 out_sigma[3]);

// The inversion this renderer uses, fitted against its own transport:
//   sigma = -ln(c) / (2.17 + 2.02 * reference_depth)
// D came out independent of the azimuthal roughness, which is why this is so
// much simpler than the published form. Round-trip error under 0.002 over
// c in [0.07, 0.85] and depth in [3, 10]. `reference_depth` is the fibre depth
// at which the authored colour is exact; shallower reads lighter, deeper
// darker, as hair does.
void HairSigmaFromColor(const f32 color[3], f32 reference_depth, f32 out_sigma[3]);

// Shipped starting points. Melanin concentrations from the ranges measured in
// human hair; each preset's reasoning is in the .cc. Starting points, not
// measurements - the look-dev bench exists so they can be replaced by fits.
enum class HairPreset : u8 { kBlack, kBrown, kBlonde, kRed, kGrey, kWhite };
HairSurfaceParameters HairPresetParams(HairPreset preset);

// Quality tiers, matching the character model's: a reduction of the same BSDF,
// never a different one.
//   Hero      full R/TT/TRT + residual, dual scattering with a measured count
//   Standard  same lobes, dual scattering from a cheaper constant-depth estimate
//   Distant   single lobe, no multiple scattering, no per-fragment h
enum class HairTier : u8 { kHero, kStandard, kDistant };
struct HairTierCaps {
  bool dual_scattering = true;
  bool transmittance_volume = true;  // false = assume a constant fibre depth
  bool per_fragment_h = true;
  bool tilted_lobes = true;
};

// Absorption for a fibre whose authored colour is `color`, honouring the
// material's colour mode. Shared by the CPU mirror and (mirrored) by the
// shader, so both derive it the same way.
void HairResolveSigma(const HairSurfaceParameters& params, const f32 color[3], f32 out_sigma[3]);
HairTierCaps HairTierApply(HairTier tier, HairSurfaceParameters& params);

// Safe authoring ranges; the bench clamps to these and the docs publish them.
struct HairRange {
  f32 lo;
  f32 hi;
};
HairRange HairSafeRange(const char* field);

// --- CPU mirror of hair_bsdf.hlsli ------------------------------------------
// Kept equivalent to the shader by construction. hair_bsdf_test diffs its
// behaviour against the contracts the shader is supposed to hold.

// wo/wi in the strand's local frame (+x along the tangent). Returns the BSDF
// without the cosine, like the shader's HairEvaluate. NOT reciprocal - the
// attenuations derive from the outgoing direction alone, as in the published
// model. See the note in hair_bsdf.hlsli.
void HairEvaluateCpu(const HairSurfaceParameters& p, const f32 wo[3], const f32 wi[3], f32 h,
                     f32 out_rgb[3]);

// Cosine-weighted, with dual scattering, like the shader's HairShade.
void HairShadeCpu(const HairSurfaceParameters& p, const f32 wo[3], const f32 wi[3], f32 h,
                  f32 strand_count, f32 out_rgb[3]);

// White-furnace style check: integrates the single-scattering BSDF over the
// sphere of outgoing directions for a fixed incoming one. A non-absorbing fibre
// must come out at or below 1 (energy conserving) and close to 1 (not lossy).
f32 HairAlbedoCpu(const HairSurfaceParameters& p, const f32 wo[3], u32 theta_steps,
                  u32 phi_steps, int channel);

}  // namespace rx::render

#endif  // RX_RENDER_HAIR_MATERIAL_H_
