# Lens flare correction, 2026-09-12

The production tonemap shader reproduced oversized object ghosts and exposure-dependent flare intensity. The flare source is a tight, quarter-resolution bloom snapshot. The defects were in how the final pass selected and sampled it.

## Corrections

- Ghost gathering multiplied coordinates by fractional scales, magnifying source silhouettes. It now divides by each desired destination scale around the optical center.
- A separate halo sampled a fixed circle and drew a screen-centered ring regardless of source position. That ring was removed.
- The previous per-channel threshold accepted ordinary bright surfaces. Selection now uses exposed luminance with a threshold of 4 and a soft knee from 3 to 5, preserving highlight color ratios.
- Flare compression happened before scene exposure, causing different results for equivalent exposed highlights. Extraction and compression now operate in exposed space, and the flare is added to the exposed scene.
- Out-of-frame source coordinates are rejected instead of extending edge highlights through the clamp sampler. A source-edge fade avoids a hard cutoff.

The intensity control and bloom remain available. Strong highlights still produce ghosts. This remains a screen-space artistic effect, not a physical lens simulation.

## Regression coverage

`lens_flare_test` renders the actual `PostPass` and production tonemap shader into a floating-point target. It checks ordinary bright-object rejection, strong-highlight response, compact ghost placement, exposure invariance, empty input, disabled flares, and an edge-of-frame source.

Before the fix, the ordinary-object case produced summed output energy 123.43 instead of zero. The strong-source case placed all measured energy outside the intended ghost footprints. Equivalent exposed inputs differed by up to 0.562665 per channel. All three assertions failed against the original shader.

After the fix, ordinary objects produce zero flare energy, strong sources remain visible, energy outside the expected ghost footprints is zero, and equivalent exposed inputs match exactly. The edge-source case also produces no out-of-footprint energy. These cases pass on NVIDIA Vulkan, D3D12 through vkd3d, and llvmpipe Vulkan.

## Verification and limits

The build passed. The full 147-entry CTest run passed 146 tests, including all renderer tests. An unrelated vehicle-audio mailbox stress test exceeded its sample-delta limit (0.1536 against 0.15); its isolated rerun passed (0.0888). No audio code was changed.

Four before and four after captures cover the lights and materials scenes at flare intensities 0 and 0.06. All captures completed without validation errors. The lights scene's oversized patches disappear, and its disabled-flare before/after captures are pixel-identical. The materials captures have small run-to-run differences even with flares disabled (mean absolute difference 0.00783 in 8-bit RGB units), so they are visual checks rather than exact image regressions. Strong-highlight preservation is established by the deterministic GPU test.

Artifacts and logs are under `build/bugbash/flare-before` and `build/bugbash/flare-after`. The user's exact scene was not supplied. Bloom can still create a local glow, and large emissive sources can still produce visible artistic ghosts.
