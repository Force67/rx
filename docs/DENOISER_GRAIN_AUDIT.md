# Denoiser grain investigation, 2026-09-12

The investigation reproduced persistent grain in the materials scene with RR, NRD, and SVGF, then separated post-processing grain from residual path-sampling noise. The user's exact scene and active denoiser were not specified.

## Film grain after denoising

`tonemap.ps.hlsl` adds random film grain after reconstruction and tonemapping, before output encoding. The default amplitude was 0.015. No denoiser can remove this later addition; it remains visible even on a converged sky or smooth surface.

Matched 320x180 captures at 64 frames isolate this contribution. Horizontal adjacent-pixel RMS difference in the sky rectangle x=20..299, y=8..54, measured in 8-bit RGB units:

| Mode | Film grain 0.015 | Film grain 0 |
| --- | ---: | ---: |
| SVGF | 1.915 | 0.174 |
| RR | 1.892 | 0.177 |
| NRD | 2.303 | 0.163 |

This statistic includes the underlying sky gradient and is not a general denoiser-quality score. The matched captures visibly confirm the post-processing speckling.

Fix: film grain defaults to zero in `RenderSettings` and the environment-option declaration. The existing Film grain slider and `RX_FILM_GRAIN` override remain available. Explicitly authored settings, including the feature gym's post-effects demonstration, can still enable it. The new default capture is pixel-identical to an explicit zero-grain capture.

## SVGF reflection filtering was nearly disabled

The specular a-trous kernel multiplied neighboring weights by `exp(-distanceSquared * (1 - roughness)^2 * 8)`. At roughness 0.2, even a one-pixel neighbor gets only 0.006 of its otherwise valid weight. Later passes use larger distances and reject practically all neighbors. Four filter passes consequently retained almost all noise on a flat glossy surface.

Fix: express the spatial limit as a Gaussian radius in pixels, `max(roughness * 8, 1)`, with weight `exp(-distanceSquared / (2 * radiusSquared))`. Normal, depth, and variance-driven luminance rejection still protect boundaries. This changes SVGF's specular filter; it does not change the RR or NRD kernels.

The new `recon_atrous_test` dispatches the production shader over a 64x64 flat reflection with known mean 1 and random noise. Four passes gave RMS error 0.284824 before the fix and 0.0756689 afterward, a 73.4% reduction. The output mean is 0.997410. A separate noiseless reflection edge and a depth discontinuity have RMS errors below 0.0005 after filtering. The old implementation fails the noise-reduction assertion; the new one passes all three cases on Vulkan and D3D12.

The variance-driven filtering structure follows [SVGF's temporal and spatial reconstruction approach](https://research.nvidia.com/publication/2017-07_spatiotemporal-variance-guided-filtering-real-time-reconstruction-path-traced). The bounded specular radius here is an engine-specific choice, validated by these regressions rather than claimed as the paper's original kernel.

## Verification and limits

The build and eight focused tests passed, including the Vulkan/D3D12 reflection and temporal tests, traced motion, allocation failures, and actual DLSS/RR integration. Seven post-change materials captures passed with zero Vulkan validation or SDK evaluation errors. RR captures require successful RR activation, so fallback does not count as an RR pass. Captures from all three denoisers were visually inspected.

The full 145-entry CTest suite passed. The temporal and reflection regressions also passed on llvmpipe; the reflection-noise RMS there is 0.0756703. `git diff --check` passed.

Artifacts, before/after captures, numerical measurements, and logs are in `build/bugbash/denoiser-before` and `build/bugbash/denoiser-after`.

These fixes address two demonstrated contributors. Freshly revealed surfaces, rapidly changing lighting, and low-sample reflections can still contain Monte Carlo noise. Reference mode intentionally bypasses denoising. The spatial-filter change trades some local glossy detail for noise reduction; the edge regressions cover the constructed cases, not every reflection or camera motion.
