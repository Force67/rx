# Motion, upscaling, and transport audit, 2026-09-12

This follow-up checks estimator behavior and temporal inputs in addition to API validity. It covers traced primary motion, skinned vertex history, surface identity, reference accumulation, GI reconnection, DLSS Super Resolution, FSR3 upscaling, and NGX Ray Reconstruction.

## Confirmed fixes

| Area | Correction |
| --- | --- |
| Traced rigid motion | Reproject the hit's barycentric surface point through the previous object transform. Previously only camera motion was represented. |
| Traced skin motion | Read the previous posed vertex buffer at the hit triangle's barycentrics. The first pose cannot expose uninitialized previous vertices. |
| Surface identity | Carry CPU history IDs in metadata ordered exactly like the built TLAS, including BLAS omissions. Correspondence checks invalidate new, reordered, or replaced geometry instead of treating its transient TLAS index as persistent identity. |
| Reference accumulation | Reset after transforms, instance membership, or bone poses change. Geometry changes also reset DI/GI reservoirs because their stored secondary samples are stale. TLAS reservation failures discard scene history before recovery. |
| GI temporal reconnection | Convert reused samples from the previous visible point's solid-angle measure to the current one using the reconnection Jacobian. Previous primary positions are now retained. |
| Dim paths | Replace the deterministic throughput cutoff below 0.01 with Russian roulette and reciprocal survival weighting in all three path tracers. |
| Raster sky jitter | Subtract the projection offset during unprojection, matching the mesh vertex shader's positive clip-space offset. The old sign moved sky samples opposite geometry. |
| DLSS timing | Supply frame duration in milliseconds to evaluation. |
| Upscaler history | Reset DLSS and FSR histories on first use, skipped frame indices, and requested cuts. Evaluation errors invalidate subsequent history. |
| Camera cuts | Expose `FrameView::camera_cut` and route it through first-frame resets, TAA, path tracing, and upscaling. Callers must set it for discontinuous camera changes. |
| RR activation | Set the required input-resolution motion-vector flag, including native-resolution mode. Without it NGX returned `0xbad00005` with `Low resolution Motion Vectors required`, so RR always fell back to SVGF on the tested driver. |
| Unused graph textures | Do not allocate transients that no pass accesses. Once RR activated, unused SVGF scratch images otherwise caused zero-usage Vulkan image creation and graph failure. This also covers one-pass a-trous configurations. |
| RR sampling | Supply a 32-position subpixel jitter sequence and trace the corresponding primary rays. NGX motion remains unjittered; internal DI/GI reservoir reprojection separately adds previous-minus-current jitter. |
| RR guides | Supply integrated, view-dependent specular reflectance instead of bare F0, neutral sky albedos, and the traced specular hit distance alongside the existing matrices. |
| NGX failure cleanup | Destroy partially returned capability parameters and shut down initialization when the capability query fails. |

History matching is deliberately conservative: draw order changes may discard usable history. It does not attempt to recover arbitrary identity from indistinguishable mesh/transform pairs. Invalid correspondence emits motion outside the image, preventing accidental history reuse. Out-of-image motion is bounded to finite UV offsets so fast movement near the camera cannot overflow the RG16F target.

## Conventions verified without changing them

The existing current-to-previous UV motion direction, render-size motion scale, reversed depth, and projection-offset jitter sign are correct for the tested DLSS and FSR paths. The engine's column-vector/column-major matrix storage also matches NGX's row-vector/row-major representation; transposing those matrices would be incorrect.

The analytic SDK test feeds a known sinusoidal image at subpixel sample positions, runs 32 frames, and compares the final eight outputs against the continuous image at output resolution. Deliberately reversing inputs is a control, not a description of the original production integration.

| SDK | Correct jitter MSE | Reversed jitter MSE | Correct motion MSE | Reversed motion MSE |
| --- | ---: | ---: | ---: | ---: |
| FSR3 | 0.000553295 | 0.00407163 | 0.000318048 | 0.00754785 |
| DLSS | 0.000243668 | 0.00548857 | 0.000220676 | 0.00158442 |

Skipped-frame resets match explicit resets to the test's 0.000001 MSE tolerance for both SDKs. Readback rejects non-finite color. These are actual NVIDIA GPU SDK evaluations, not mocks or source-text assertions.

## Regression coverage

- `path_motion_test`: production reconstruction and NRD ray queries verify previous rigid and posed surface coordinates, expected UV motion, very large offscreen motion, invalid-history rejection, and metadata lookup after an earlier unbuilt instance is omitted. CPU checks cover identity, draw reorder, pose changes, and removal.
- `raytracing_test`: actual compute skinning and BLAS refits additionally verify previous-pose buffer availability and isolation from the current slot.
- `path_material_test`: public renderer calls verify accumulation after material edits, rigid motion, stopping, and a camera cut. Static/moving/stopped/cut counts are 4/2/4/2.
- `path_sampling_test`: production DI, sky, and GI temporal shaders verify jitter reprojection at an image boundary. A constructed temporal GI reconnection has Jacobian 1/4 and must return W = pi/2, approximately 1.5708. Dim-path continuation preserves expected RGB energy (0.001, 0.002, 0.003), measured as (0.000978394, 0.00195679, 0.00293518) over 16,384 samples.
- `upscaler_motion_test` and its DLSS variant: image accuracy, motion/jitter controls, finite output, and skipped-frame recovery described above. The x86 DLSS variant also requires native RR creation and resize, destroys RR, then evaluates DLSS to check shared NGX lifetime.
- `rt_failure_test`: an unused denoiser transient compiles without making any device allocation, including when the first allocation would fail.

## Sources and remaining limits

SDK contracts were checked against the bundled [DLSS Programming Guide](../third_party/DLSS/doc/DLSS_Programming_Guide_Release.pdf), [RR Integration Guide](<../third_party/DLSS/doc/DLSS-RR Integration Guide.pdf>), and SDK headers, plus AMD's [FSR upscaler documentation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/). The RR specular reflectance fit follows the guide's appendix, citing Ray Tracing Gems chapter 32.

- This is not a complete PBR ground-truth integrator. Diffuse/force-opaque reference transport, fixed ray offsets/ranges, finite bounce depth, radiance clamps, and approximate reservoir visibility remain model limits.
- Raster morph-weight changes and procedural wind/water displacement still have approximate motion. The existing raster skin path tracks previous bones, but reuses current morph weights. The traced skin path handles posed vertices; its skinning shader does not add raster-only morph or procedural displacement. These differences remain outside the confirmed fixes above.
- XeSS has no implementation in this checkout and falls back to TAA. DLSS and FSR here use Vulkan SDK integrations; native D3D12 vendor SDK integrations are not present.
- The documented golden-image harness is absent from this checkout. Numerical shader/SDK tests, dedicated captures, and the feature tour provide the available checks.
- Native Windows DXR, ARM NGX shutdown behavior, arbitrary SDK allocation failures, and frame-generation quality are not established by these tests. The earlier unsupported-memory FSR guard and ARM driver workaround remain.
- Primary-position history adds two persistent RGBA32F images, replacing the prior single transient primary-position target. Motion metadata costs 80 bytes per allocated TLAS instance per slot.

Successful validation is separate from the numerical correctness evidence above. Neither establishes that every scene or driver is bug-free.

## Verification

The full Vulkan/D3D12 build and all 143 CTest entries passed after the RR activation, graph, and finite-motion fixes. All four focused ray-query/motion/sampling/accumulation tests also passed on llvmpipe with ray queries enabled. The available WineHQ D3D12 provider ran the temporal sampling tests; spatial ray queries remain a capability skip there.

Eight 320x180 captures at 32 frames passed with zero Vulkan validation errors: reference Cornell, SVGF Cornell/materials, NRD Cornell, RR Cornell/materials, and raster DLSS/FSR materials. The RR checks require successful RR initialization, so an SVGF fallback cannot count as an RR pass. Captures were visually inspected.

The complete 32-stop feature tour passed at 1280x720 with zero Vulkan validation or SDK evaluation errors. It covers streamed instance replacement, animated geometry, TAA/MSAA, upscaler transitions, fog, and reconstructed path tracing. The final large-motion regression also passed on llvmpipe. `git diff --check` passed.

Artifacts and runner scripts are in `build/bugbash/motion-upscalers`. Changes are uncommitted.
