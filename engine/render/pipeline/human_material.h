#ifndef RX_RENDER_HUMAN_MATERIAL_H_
#define RX_RENDER_HUMAN_MATERIAL_H_

#include "asset/material.h"
#include "core/types.h"

namespace rx::render {

// CPU side of the character surface model. Three things live here:
//
//   1. HumanSurfaceParameters - the resolved, GPU-ready parameter block, and
//      the region presets that seed it. These are STARTING POINTS fitted
//      against the engine's own reference rig, not measured constants; the
//      lookdev tool exists so they get re-fitted per project.
//   2. The quality tiers (hero / standard / distant). A tier is a reduction of
//      the SAME model, never a different one - a face that changes material
//      semantics at 8 m is the defect the tiers exist to prevent.
//   3. HumanBrdf - a bit-faithful CPU mirror of human_brdf.hlsli, so the
//      neutral-parity contract and the automated fitting experiments can run
//      without a GPU.
//
// See engine/render/shaders/human_brdf.hlsli for the shader side and
// docs/CHARACTER_RENDERING.md for the workflow.

using HumanRegion = asset::Material::HumanRegion;

// Version of the character surface model's PARAMETER SEMANTICS. Bump it when a
// control changes meaning, range or default - not when the implementation
// changes. Fitted parameters are measurements against a specific model, and a
// preset silently reinterpreted under a newer one is worse than a preset that
// refuses to load: the numbers still look plausible, so nobody notices.
// History:
//   1  initial model (docs/CHARACTER_RENDERING.md)
inline constexpr u32 kHumanModelVersion = 1;

// Resolved parameters, laid out to match the shader's HumanSurfaceParams and
// the tail of MaterialSystem::Params. All lengths are metres.
struct HumanSurfaceParameters {
  f32 diffuse_fresnel_peak = 0.0f;
  f32 diffuse_fresnel_falloff = 5.0f;
  f32 diffuse_fresnel_tangent_falloff = 5.0f;

  f32 retroreflection_peak = 0.0f;
  f32 retroreflection_falloff = 5.0f;
  f32 retroreflection_tangent_falloff = 5.0f;

  f32 smooth_terminator_amount = 0.0f;
  f32 smooth_terminator_length = 0.0f;

  f32 specular_fresnel_falloff = 5.0f;
  f32 secondary_roughness_scale = 3.0f;
  f32 secondary_specular_weight = 0.0f;
  f32 light_shape_response = 0.0f;

  f32 mean_free_path = 0.001f;
  f32 subsurface_scale = 1.0f;
  f32 transmission = 0.0f;
  f32 transmission_tint[3] = {1.0f, 0.35f, 0.2f};
  f32 extinction_scale = 1.0f;
  f32 thickness_scale = 0.01f;

  f32 corneal_wetness = 0.0f;
  f32 cavity_occlusion = 0.0f;
  f32 specular_normal_strength = 1.0f;

  f32 iris_depth = 0.0028f;
  f32 iris_radius = 0.16f;
  f32 pupil_scale = 1.0f;
  f32 limbal_ring_size = 0.035f;
  f32 limbal_ring_power = 2.0f;
  f32 cornea_ior = 1.376f;
  f32 iris_shadow_depth = 0.5f;

  f32 residual_weight = 0.0f;
  HumanRegion region = HumanRegion::kSkin;
};

// The neutral set: identical output to the stock Lambert + GGX path. Anything
// that breaks this is a regression (human_brdf_test.cc asserts it).
HumanSurfaceParameters HumanNeutral();

// Fitted starting points per anatomical region. Each is a hypothesis to be
// re-fitted against project reference, and each one's rationale is in the .cc.
HumanSurfaceParameters HumanPreset(HumanRegion region);

// Quality tiers. Hero is the full model; Standard drops the second GGX lobe and
// halves transport cost; Distant collapses to the neutral model with prebaked
// normals. The reduction is monotonic so a LOD transition can only ever
// simplify, never re-shade.
enum class HumanTier : u8 { kHero, kStandard, kDistant };

// Applies a tier to a parameter set in place, and reports which optional
// features the tier permits (the shader gates on the same booleans through the
// material flags, so CPU and GPU cannot disagree).
struct HumanTierCaps {
  bool dual_specular = true;
  bool separate_normals = true;
  bool transmission = true;
  bool eye_refraction = true;
  bool residual = true;
  bool full_sss = true;  // false = half-res / simplified diffusion
};
HumanTierCaps HumanTierApply(HumanTier tier, HumanSurfaceParameters& params);

// Screen-height (in pixels) thresholds the renderer uses to pick a tier for a
// character. Hysteresis is the caller's job; these are the nominal edges.
HumanTier HumanTierForScreenHeight(f32 pixels);

// Safe authoring ranges. The lookdev UI clamps to these and
// docs/CHARACTER_RENDERING.md publishes them; outside them the model stops
// being energy-sane (documented per-field in the .cc).
struct HumanRange {
  f32 lo;
  f32 hi;
};
HumanRange HumanSafeRange(const char* field);

// Resolves an authored asset material into the GPU parameter block, applying
// the region preset for anything the material left at its neutral default.
HumanSurfaceParameters HumanResolve(const asset::Material::HumanParams& authored);

// The inverse: writes a resolved parameter block back onto an authored material
// (the look-dev tool edits the resolved form and has to hand it back). Texture
// references on the authored side are left alone.
void HumanStore(const HumanSurfaceParameters& params, asset::Material::HumanParams& authored);

// --- CPU mirror of human_brdf.hlsli -----------------------------------------
// Used by the neutral-parity test and by the offline fitting experiments. Keep
// it byte-for-byte equivalent to the shader; the test diffs both against the
// stock Lambert + GGX reference.
struct HumanBrdfSample {
  f32 diffuse[3] = {0, 0, 0};
  f32 specular[3] = {0, 0, 0};
  f32 transmission[3] = {0, 0, 0};
};

// n/l/v are unit vectors in the same space; nd/ns are the diffuse and specular
// shading normals (pass n for both when they are not split). solid_angle in
// steradians (0 = punctual). thickness in metres (0 = opaque).
HumanBrdfSample HumanEvaluateCpu(const HumanSurfaceParameters& p, const f32 base_color[3],
                                 f32 roughness, const f32 f0[3], const f32 geometric_n[3],
                                 const f32 nd[3], const f32 ns[3], const f32 v[3],
                                 const f32 l[3], f32 solid_angle, f32 thickness);

// The stock Lambert + GGX the neutral set must reproduce.
HumanBrdfSample StockBrdfCpu(const f32 base_color[3], f32 roughness, const f32 f0[3],
                             const f32 n[3], const f32 v[3], const f32 l[3]);

}  // namespace rx::render

#endif  // RX_RENDER_HUMAN_MATERIAL_H_
