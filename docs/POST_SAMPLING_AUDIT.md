# Post-processing sampling audit, 2026-09-12

Follow-up to the lens flare investigation. This pass found five correctness problems in depth of field and motion blur, including failures that produced no Vulkan validation errors.

## Depth of field

The CoC shader used output pixel coordinates to load the render-resolution depth texture. After upscaling, this reads the wrong surface or falls outside the texture. Autofocus also selected its center using the output dimensions. Both now address the actual depth extent; CoC maps pixel-center UVs to nearest depth texels, preserving depth boundaries.

Autofocus wrote shared focus state from thread (0,0) while every other thread read it in the same dispatch. There was no device-wide ordering between those workgroups. On the first frame, some pixels used the initial zero focus state, clamped to the near plane; during adaptation, pixels could disagree about the current focus distance. Focus now updates in a single-thread pass into a persistent R32F image. The graph declares the dependency before the CoC pass reads it, including the dependency on the previous frame's focus state. This also removes the untracked host-visible storage buffer.

An explicit focus distance was still interpolated using autofocus speed. Changing the manual distance therefore did not immediately focus at that distance. The explicit override now applies directly; only autofocus eases toward its target.

## Motion blur

Velocity tiles covered 16x16 output pixels, but their reduction loaded 16x16 render pixels at the same integer coordinates. Upscaling displaced the dominant velocities into the wrong tiles. Tile construction now maps output pixel centers into the motion texture's actual dimensions.

Both the tile reduction and the 3x3 neighborhood selection compared UV-vector lengths. On a 256x128 image, an 8-pixel vertical vector could beat a 12-pixel horizontal vector. Both comparisons now use output-pixel lengths, while storing and sampling the vectors in UV units.

## Numerical regressions

`post_sampling_test` executes the production depth-of-field and motion-blur graph passes and reads back their output. It also dispatches the production tile reduction shader to check the chosen vector directly.

Initial checks against the original code produced:

| Check | Maximum error before | After |
| --- | ---: | ---: |
| Native in-focus image unchanged | 0.699951 | 0 |
| Upscaled in-focus image unchanged | 0.699951 | 0 |
| Uniform autofocus update against fixed-focus control | 0.232178 | 0 |
| Explicit focus-distance change | 0.506592 | 0 |
| Upscaled moving-region blur against native control | 0.480469 | 0 |
| Dominant vector selection (sum of UV component errors) | 0.109375 | 0 |

The autofocus race is schedule-dependent; its measured error is evidence of a failing run, not a guaranteed error magnitude. The motion test also requires a visible blur response, so simply disabling the effect cannot pass. Additional checks cover native and upscaled autofocus, a static motion field, and aspect-correct neighborhood selection.

## Verification

The full build and all 149 CTest entries passed. All eleven post-sampling checks passed on NVIDIA Vulkan, D3D12 through vkd3d, and llvmpipe Vulkan. The focused runs enabled synchronization validation. `git diff --check` passed.

Six before and six after materials captures cover depth of field off/on at native resolution and with FSR3/DLSS upscaling. The high preset keeps the SDKs at 426x240 input and 640x360 output, confirmed in their activation logs. Every capture completed without validation or SDK evaluation errors. The corrected upscaled captures visibly recover the intended object boundaries; the previous shader blurred displaced silhouettes across them. Native-resolution depth-of-field captures differ by at most one 8-bit RGB unit. Disabled-effect captures have small run-to-run lighting differences (maximum four units), so they are not treated as exact image regressions.

Logs, scripts, measurements, and images are under `build/bugbash/post-sampling-before` and `build/bugbash/post-sampling-after`. The repository's documented golden harness is absent; these GPU assertions and captures provide the available verification.

These changes preserve the existing approximate bokeh and motion-gather models. They do not establish physically accurate lens blur, complete foreground occlusion handling, or an exhaustive absence of renderer bugs.
