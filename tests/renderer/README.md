# Renderer correctness gate

Vulkan validation checks API use and synchronization. It cannot tell whether a
shader samples the wrong pixel, biases a probability distribution, or produces
the wrong motion vector. This gate runs production-shader numerical regressions
and requires the selected coverage to execute.

From an already configured checkout:

```sh
nix develop -c python3 tests/renderer/check.py --runner swrun
```

The default `portable` profile builds ten tests and runs them on software
Vulkan. No NVIDIA GPU or vendor SDK is required for this profile. Python uses
only its standard library. Configure the build with `RX_BUILD_TESTS=ON` first
(`nix develop -c cmake --preset linux`).

On an NVIDIA machine with the SDKs and D3D12 backend built:

```sh
nix develop -c python3 tests/renderer/check.py --runner vkrun --profile all
```

Profiles can also be selected individually or combined with repeated `--profile`:

| Profile | Required coverage |
| --- | --- |
| `portable` | Offscreen draw/readback, RT allocation/culling/slot bookkeeping, temporal rejection, reflection filtering, alpha selection, cloud lighting, lens flares, depth of field and motion blur |
| `raytracing` | Ray queries, sampling/proposal energy and reservoir counts, traced rigid/skinned motion, material/geometry/camera-cut accumulation resets |
| `d3d12` | Seven shared draw/shader regressions through D3D12; no DXR claim |
| `fsr` | Actual FSR reconstruction with correct/reversed motion and jitter controls, plus history resets |
| `dlss` | Actual DLSS reconstruction controls and resets, plus RR creation/resize/shared-NGX lifecycle |

`all` requires every profile. It deliberately fails on hardware or builds that
cannot provide all of them. Choose profiles according to the runner's intended
coverage, not to hide a test failure. NRD-specific alpha/motion wrappers run when
NRD is compiled in; this gate does not yet provide a dedicated NRD denoising
quality oracle or an RR image-quality oracle.

## Failure and artifacts

The gate rejects build errors, missing test registrations, failures, timeouts,
missing results, and skipped tests. Exit-zero `SKIP` diagnostics are rejected too.
The tests in these profiles use exit code 77 to report unavailable coverage to
ordinary CTest. A skip is useful information, but it is not evidence that the
feature works; this convention has not been applied to every test in the repo.

Build/test logs, the selected tests and CTest inventory, and a JUnit report are
saved under `build/linux/renderer-check/<profiles>/`. Use `--out` to select a CI
artifact directory, `--build-dir` for another configured build, or `--no-build`
after rebuilding separately. Tests run serially with synchronization validation
enabled. Local `RX_*` overrides are cleared; CTest supplies each backend/SDK
selection. Each invocation first runs `test_check.py` to check the gate's failure
handling; those checks are also registered in CTest when Python is available.

There is currently no CI service configuration in this checkout. These commands
are suitable as required CI jobs: `portable` on a software Vulkan runner for
each renderer change, and the hardware profiles on suitable GPU runners before
merging. Run the feature-gym tour for integration changes and on the GPU runner's
regular scene regression job. Upload the entire output directory even when the
command fails. The script's nonzero exit status must fail the job. No remote CI jobs or branch
protection have been installed by adding this gate.

## Rules for future renderer changes

For a numerical or temporal bug, add a small input that fails against the old
code and passes against the correction. Execute the production shader or pass;
do not test a second copy of the same formula. Prefer known values or independent
controls over fragile whole-frame image equality.

Useful properties already covered here include equivalent exposed inputs giving
the same flare, an in-focus image remaining unchanged, native/upscaled blur
agreeing in the moving region, longer pixel-space velocities winning, unbiased
sampling energy, and denoising reducing noise while preserving tested edges.
Cloud lighting is checked by recovering transmission from black/white background
pairs: changing density must change opacity without changing the scattering
source. The production pass also covers foreground occlusion, empty clouds,
zero illumination, and odd output dimensions on both backends.

Extend those tests when changing a pass's inputs or behavior. Cover applicable
resolution changes (including odd sizes/aspect ratios), motion/jitter signs,
history resets, empty or zero-energy inputs, and feature on/off behavior. A
passing Vulkan validation run alone does not satisfy a visual correctness fix.

Run the [feature-gym tour](../feature_gym/README.md) for scene-level integration
checks and inspect intentionally changed captures. The tour now saves logs and
rejects validation/SDK evaluation errors. Its black/uniform-image checks remain
smoke tests, not an aesthetic-quality oracle. Review changes in halos, ghosting,
blur, and noise directly; do not bless a new reference image merely to turn a
failure green.
