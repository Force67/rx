# Render-pass sampling, 2026-09-12

These measurements isolate the noise optimization. The subsequent cloud-lighting
fix removes an extra density multiplier from scattering and intentionally changes
cloud brightness; the image comparisons below describe the optimization alone.

Baseline: `d940c81`, with the timing-file export added to both builds.
Hardware: RTX 3080 Ti, Vulkan, RelWithDebInfo, high preset, native 1920x1080
with `--no-taa`. Captures use the fixed 1/60-second clock. The desktop remained
active, so GPU preemption made means and p95 substantially noisier than medians.

The initial 240-frame samples of materials, city, and Cornell ranked procedural
clouds highest (mean 1.45 to 3.13 ms). Reflection denoising followed at about
1.3 ms. The city scene's opaque pass also cost about 1.25 ms. Cloud shadows were
a smaller, related opportunity at a median 0.112 ms in materials.

The changes skip noise octaves when their maximum remaining contribution cannot
produce positive cloud density. Erosion also stops once the partial sum already
erases the sample. Surviving samples retain their original noise octaves, march
steps, and lighting. Cloud shadows use the same upper-bound optimization with
their existing noise function.

The longer comparisons captured frame 480, discarded the first 120 resolved
frames, and retained 359 frames per run. These are median pass times, not FPS:

| Pass and scene | Before (ms) | After (ms) | Reduction |
| --- | ---: | ---: | ---: |
| Clouds, materials | 3.300 | 2.645 | 20% |
| Clouds, city | 1.934 | 1.742 | 10% |
| Clouds, materials, coverage 0.2 | 2.004 | 0.848 | 58% |
| Clouds, materials, coverage 0.85 | 3.247 | 3.191 | 2% |
| Cloud shadows, materials | 0.112 | 0.062 | 44% |

The dense-cloud difference is small enough to treat as unchanged. Earlier
default-coverage runs also measured roughly 15 to 20% less cloud-pass time.
Per-pass timestamps include barriers and perturb scheduling; use whole-frame
timestamps to check overall impact.

With per-pass timestamps disabled, two alternating-order whole-frame comparisons
gave materials medians of 9.173 to 7.822 ms and 11.697 to 10.884 ms (15% and 7%
reductions). City measured 12.072 to 11.585 ms and 12.370 to 11.520 ms (4% and 7%).
Absolute timings varied with desktop load; these are local observations rather
than a guaranteed frame-rate increase on other hardware or scenes.

Reproduce a pass sample:

```sh
nix develop -c vkrun env RX_GPU_TIMINGS=1 RX_GPU_TIMINGS_FILE=/tmp/passes.tsv \
    build/linux/runtime/rx --demo materials --preset high --no-taa --headless \
    --width 1920 --height 1080 --shot /tmp/materials.png --shot-frames 480
python3 tools/gpu_timings.py /tmp/passes.tsv --warmup 120
```

Use `--scene runtime/scenes/city.rxscene` for city, `--demo cornell` for Cornell,
or `RX_CLOUD_COVERAGE=0.2` / `0.85` for the coverage checks. Use
`RX_GPU_TIMINGS=0` for the whole-frame bracket. Repeat comparisons in alternating
build order to limit drift from desktop load and temperature.

The four longer cloud comparisons passed `rxdiff`, with RMSE between 0.00012
and 0.00023 against the existing 0.002 limit. The cloud-shadow comparison passed
at 0.00017. No image references were updated.

Verification:

- Built both Vulkan/SPIR-V and D3D12/DXIL paths.
- Portable renderer gate: all 9 tests passed on `swrun`.
- Hardware renderer gate: all 21 tests passed with all profiles on `vkrun`,
  including ray tracing, D3D12, FSR, and DLSS coverage; no skips.
- Before/after captures without ray tracing at 641x359 passed on both Vulkan
  and D3D12. The Vulkan result was pixel-identical; D3D12 RMSE was 0.00037.
- The clouds-disabled Vulkan capture also passed. No validation errors were
  reported by these capture runs.
- The Vulkan/D3D12 comparison passed at RMSE 0.00134.
- The feature-gym tour passed all 32 captures with validation enabled.
