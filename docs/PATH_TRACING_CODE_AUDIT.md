# Path-tracing code scan, 2026-09-12

Follow-up to the renderer bug bash. The confirmed findings below have been fixed, with regressions that failed against the original code. The follow-up estimator audit found an additional normalization bug that API validation did not detect. The scan inspected the reference and NRD path tracers, reconstruction gbuffer, GI/DI reservoir stages, sky CDF, temporal/a-trous/composite/fog shaders, and the associated CPU setup and material update paths.

## Confirmed findings

### P1: ReSTIR DI incorrectly normalizes sun and point-light proposals

`engine/render/shaders/gi/recon_restir_di_temporal.cs.hlsl:223`

The initial reservoir takes one sun sample with weight `p_hat_sun` and K point-light samples with weight `p_hat_light * light_count`, then divides by M = K + 1. These proposals cover disjoint light classes. Averaging their estimates does not estimate the sum of both classes.

Source-level reproduction on a reset frame: use a unit sun target and a point light whose target is zero at the receiving pixel. With the production K = 8, the selected sun has `w_sum = 1`, `M = 9`, and `W = 1/9`. Spatial reuse of identical reservoirs preserves this error. Conversely, with zero sun and a unit point-light target, the result is 8/9 instead of 1.

Fix: include each proposal's share of the candidate set in its PDF. The sun weight is multiplied by K + 1, and each point-light weight by (K + 1) / K. Zero point candidates preserve the sun-only estimate. The relevant sample-count weighting is described in [PBRT's multiple importance sampling discussion](https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency).

### P1: Textureless alpha masks bypass the cutoff

`engine/render/shaders/gi/recon_gbuffer.cs.hlsl:146`

`PassesAlpha` returns true when `base_color_texture == 0xffffffffu`, even when the material is alpha-masked and its base-color alpha is below `alpha_cutoff`. The same early return exists in `pathtrace_gbuffer.cs.hlsl:128` and `recon_restir_di_spatial.cs.hlsl:130`.

The CPU material record preserves the alpha factor, mask flag, and cutoff without requiring a texture (`MaterialSystem::BuildBindlessRecord`). A textureless mask with alpha 0 and cutoff 0.5 therefore becomes a visible surface and shadow blocker in these paths. The absent texture should contribute alpha 1 while the factor still participates in the cutoff test.

Fix: masked materials without a base-color texture compare the factor alpha directly against the cutoff in all three shaders. The reference shader's force-opaque behavior is explicitly intentional and is a separate limitation.

### P1: Live material edits do not invalidate reference accumulation

`engine/render/core/renderer.cc:2739`

`Renderer::UpdateMaterial` updates the material buffers without advancing `scene_revision_` or otherwise resetting the path tracer. Reference reset at line 5085 checks camera motion, sun changes, scene revision, and mode transitions. A stationary camera with a fixed sun therefore keeps averaging samples from before and after an albedo or emissive edit.

For example, after 1000 samples of an emissive surface, setting its emission to zero leaves the old energy in the persistent sum. The first two new samples retain approximately 1000/1002 of the old accumulated contribution. Fix: successful material updates advance the scene revision, resetting reference accumulation and reconstructed history. Failed updates leave history intact.

### P2: Sky cell sampling disagrees with its solid-angle PDF

`engine/render/shaders/gi/recon_restir_di_temporal.cs.hlsl:148`

The CDF weights cells by luminance times solid angle. The sampler reports `pdf = luminance / total`, which requires uniform solid-angle sampling within the selected cell. It instead samples theta uniformly.

Source-level reproduction for the north-pole row of the 64-row grid: half the draws fall into the first half of the theta interval, which occupies only 25.0038% of that cell's solid angle. Fix: sample cos(theta) uniformly between the row boundaries to match the reported PDF. See [PBRT's directional sampling derivation](https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions).

### P1: Reservoir reuse excludes zero-contribution samples from normalization

The reconstruction DI, sky, and GI temporal/spatial stages gated the sample count on a positive reservoir weight. A valid zero-contribution sample was therefore treated as though it had never been drawn. GI spatial reuse also omitted counts when a reused sample had zero target at the receiving point.

Production-shader reproduction: the fresh DI estimate is 1 with M = 2. Feed equally likely histories estimating 0 and 2, both with M = 2. Correct merging gives 0.5 and 1.5, averaging to 1. Before the fix, the shader returned 1 and 1.5, averaging to 1.25. This is a 25% energy error in the constructed case, with no API misuse required.

Fix: after surface validation, retain the zero-contribution reservoir's sample count in the denominator while preventing its sample from being selected. Apply this to DI/sky and GI temporal/spatial reuse. GI's existing rejection of extreme reconnection Jacobians remains unchanged. This count treatment agrees with [the ReSTIR reservoir-combination algorithm](https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf) and [RTXDI's visibility and reservoir bookkeeping](https://github.com/NVIDIA-RTX/RTXDI-Library/blob/main/Include/Rtxdi/DI/Reservoir.hlsli).

The extended `path_sampling_test` reproduced seven failures before the fix: the DI mean-energy check plus sample-count checks for all six reuse paths. After the fix, the mean is exactly 1 in this case. The DI/GI temporal checks pass on Vulkan and D3D12. Spatial checks execute against an empty TLAS on NVIDIA Vulkan and llvmpipe; they skip on the available D3D12 provider because it lacks ray queries.

This fixes the demonstrated selection bias. It does not make the whole ReSTIR implementation unbiased: approximate surface validation and visibility reuse still require separate evaluation. Temporal GI reconnection is corrected in the motion/upscaler follow-up below.

## Further concerns and model limits

- The original camera-only motion and TLAS-index history identity findings are fixed in the [motion/upscaler follow-up](MOTION_UPSCALER_AUDIT.md), with production ray-query regressions.
- The reference integrator deliberately uses diffuse continuation and force-opaque visibility. It cannot serve as a complete PBR reference for the reconstruction path's metallic/specular materials, transparency, or transmission.
- Fixed ray offsets, fixed 1000-unit ray limits, finite bounce limits, and radiance clamps remain approximations. The dim-throughput cutoff is replaced with energy-compensated Russian roulette in the follow-up. This scan does not establish their suitability for every scene scale.

## Regression coverage

- `path_sampling_test` dispatches the production DI temporal shader over 16,384 pixels. It checks sun-only, point-only, and combined energy for 1, 4, and 8 point proposals, plus zero proposals and zero lights. Before the fix, all nine mixed-proposal cases failed. A polar sky cell tests the distribution in solid angle; before the fix its normalized mean was 0.333813 and its lower-half fraction was 0.705933, instead of 0.5 each.
- `path_alpha_test` includes the production reconstruction, NRD, and DI shadow shaders, calling each original `PassesAlpha` function with eight material cases. Before the fix, each shader incorrectly accepted three masks below their cutoffs, nine failures total.
- `path_material_test` renders through the public renderer API, edits a material, and checks accumulation reset, subsequent accumulation, and an unsuccessful update. Before the fix, the sample counts were 4, 6, 8, 10; the required sequence is 4, 2, 4, 6.

These tests cover the confirmed failures. They do not establish that the further concerns above are resolved, nor do successful API validation or visual spot checks establish transport correctness.


## Verification after the fixes

- Full build passed with Vulkan and D3D12 shader compilation, including NRD.
- All 140 CTest entries passed under the NVIDIA runner with synchronization validation enabled. The new sampling and alpha tests executed on both Vulkan and D3D12. The existing D3D12 ray-query tests skipped because WineHQ vkd3d does not expose DXR.
- All three new regressions also passed on llvmpipe. The material-history test ran with ray queries enabled and produced sample counts 4, 2, 4, 6.
- The corrected polar-cell normalized mean was approximately 0.5002. The test separately checks sample retention because direction-to-cell rounding can discard boundary samples into a neighboring dark cell (one of 16,384 on llvmpipe and D3D12). It permits at most eight such boundary losses and excludes empty reservoirs from the directional statistics.
- Four 320x180 captures at 24 frames passed with zero Vulkan validation errors: reference Cornell, reconstruction Cornell, reconstruction materials, and NRD Cornell. Images were visually inspected. Corrected DI energy and sky sampling intentionally change reconstructed lighting; pixel identity is not expected.
- `git diff --check` passed. Logs, captures, and the capture script are in `build/bugbash/path-fixes`.

Native Windows DXR remains untested. The later motion/history fixes are documented separately; the integrator approximations listed above remain.


## Estimator follow-up verification

After the reservoir-count fix, the full build and all 140 CTest entries passed again. The extended production-shader regression passed on NVIDIA Vulkan and llvmpipe, including the spatial ray-query pipelines; D3D12 executed the temporal checks and skipped the unavailable spatial ray-query checks. Four reference, reconstruction, and NRD captures completed successfully with zero Vulkan validation errors. The energy and sample-count assertions are the correctness evidence for this fix; captures and API validation provide separate integration checks. Artifacts are in `build/bugbash/reuse-fixes`.

## Motion and transport follow-up

The [motion/upscaler audit](MOTION_UPSCALER_AUDIT.md) fixes traced rigid/skinned motion, surface-history identity, reference accumulation after geometry/pose changes, temporal GI reconnection, and deterministic loss of dim-path energy. It also audits DLSS, FSR3, and NGX Ray Reconstruction against actual SDK image outputs.
