# Renderer bug bash, 2026-09-12

The audit concentrated on acceleration structures, animated geometry, path tracing, reconstruction histories, renderer fallbacks, and synchronization. It also followed validation failures into the DLSS integration and virtual-texture feedback path.

## Fixes

| Area | Failure corrected |
| --- | --- |
| TLAS lifetime | Growing a previously built slot now invalidates it until its replacement is built. Queries use the built empty fallback in the meantime. |
| TLAS bounds | Invalid slot indices cannot index past the slot arrays. Excessive capacity requests cannot wrap the growth loop. Zero-sized backend size queries fail cleanly. |
| Skinned BLAS | Reserved but unbuilt structures stay out of the TLAS. |
| Skinned actor lifetime | Duplicate release cannot put the same handle into the free list twice. Released handles cannot be prepared. |
| Skinned frame state | Actors omitted from a frame lose their active state and cannot expose old posed geometry. Requests without a usable bone palette are skipped. |
| Mesh replacement | Replacing or removing a mesh retires its posed buffers, bindless entries, and refittable BLASes while preserving acquired actor handles. |
| Allocation failures | Path tracers, hybrid reflection tracing, and ReSTIR clean up partial initialization and cannot advertise partially allocated resources as available. Zero-sized resizes remain unavailable. |
| Renderer fallback | Failed trace passes cannot be scheduled solely because the device supports ray queries. Reconstruction falls back to a usable path; unavailable tracing preserves the raster path. |
| Temporal history | ReSTIR, fog, external denoiser changes, and skipped frames reset reconstruction history. Hybrid direct-light ReSTIR also rejects history after skipped frames. |
| HDR reconstruction | Squared luminance uses FP32 moments. Non-finite history is rejected, and variance stored in half precision is bounded. Invalid history-length and blend-weight inputs cannot produce division by zero or extrapolation. |
| Reconstruction controls | A-trous iteration counts are bounded to prevent excessive work and undefined shifts. Reservoir debug views correctly enable ReSTIR. |
| Reference accumulation | Sun direction and angular radius changes now reset accumulation. Exact lighting-field comparisons replace a collision-prone weighted sum. Sample-count overflow resets accumulation. |
| Visibility rays | Very short ReSTIR visibility connections cannot issue rays with an invalid distance interval. |
| Instance culling | Setting changes and mesh-bound changes invalidate cached visibility. Sheared transforms use a conservative sphere-stretch bound. |
| D3D12 BLAS geometry | Sizing and building share one descriptor conversion, honor the vertex format, and use the required no-index format for triangle soup. |
| Async failure | Failure to acquire an async command list records the scheduled work on the main list instead of dropping it. |
| Missing async join | A graph without a valid join records serially, preventing an early segment from accessing the backbuffer before the acquire wait. |
| Mapped Vulkan memory | Persistent mapped allocations require coherent memory, matching callers that directly update frame and bindless data without flushing every write. |
| Virtual-texture feedback | The feedback copy completes its read before the counter reset overwrites the buffer. |
| DLSS/RR graph accesses | SDK output declarations include transfer clears, with the matching image usage and synchronization scope. The added RHI state maps to UAV access on D3D12. |
| NGX lifetime | The existing ARM shutdown workaround is restricted to ARM. Other platforms destroy capability parameters and shut down NGX on the last release. An existing context cannot be reused with a different Vulkan device. |
| Presentation | Acquire waits cover image layout transitions as well as rendering and transfer work, including frame generation. |
| Windowed captures | Screenshots, sequences, and real-frame generation dumps copy the backbuffer before presentation. Readback uses an owned image, avoiding access to images already released to the presentation engine. |
| FSR3 fallback | Devices without a non-host-visible device-local memory type are rejected before entering the bundled SDK allocator, which rejects those devices and leaks partial allocations on failure. |

D3D12 descriptor requirements were checked against [Microsoft's triangle geometry documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_raytracing_geometry_triangles_desc). The coherent memory requirement is supported by [Vulkan's memory-type guarantees](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryProperties.html). NGX parameter ownership was checked in the bundled `nvsdk_ngx_vk.h`.

## Regression coverage

- `rt_failure_test`: injected pipeline, camera-buffer, history-image, and TLAS allocation failures; cleanup and retry; zero-sized resizes; invalid slots; unbuilt BLAS exclusion; duplicate handle release; reconstruction mode changes; async command-list failure and missing join markers.
- `raytracing_test`: actual GPU hit distances through compacted BLASes, replacement and empty TLASes, cross-slot refits, compute-skinned poses, omitted actors, and mesh invalidation/recreation.
- `recon_temporal_test`: GPU readback of HDR second moments and half-precision outputs, including infinite history, negative history lengths, and invalid blend controls.
- Extended CPU culling and TLAS-slot regression tests.

## Verification

The full build completed inside `nix develop`, with Vulkan, D3D12, and the null backend enabled. SPIR-V and DXIL shader variants compiled.

Earlier full runs passed all 135 CTest entries with the NVIDIA Vulkan runner and synchronization validation enabled. The final full run passed 134 entries; the unchanged `vehicle_audio_test` mailbox stress check reported a sample delta of 0.158 against its 0.15 limit, then passed an isolated rerun. All renderer tests passed. The configured WineHQ vkd3d provider does not expose ray tracing, so its two acceleration-structure tests explicitly skipped. The D3D12 HDR reconstruction test ran and passed.

The offscreen, ray-tracing, and HDR reconstruction tests also passed under `swrun`. A separate ray-query run confirmed actual hits and animated poses on llvmpipe, with ray queries enabled, rather than a capability skip.

Seven deterministic scene captures exited successfully with zero Vulkan validation errors:

- Reference path tracing, Cornell scene.
- Reconstructed path tracing with ReSTIR enabled and disabled.
- Hybrid rendering with async TLAS building enabled and disabled.
- Materials scene without RT on Vulkan and D3D12.

The reference capture was pixel-identical to the pre-change capture. The reconstruction capture changed by at most 2/255 per channel, with a mean absolute difference of about 0.006/255. No golden references were regenerated.

The initial scene checks reproduced virtual-texture synchronization errors, DLSS output clear errors, and x86 NGX teardown failures. The same capture matrix passed after the fixes.

The software renderer's no-RT capture passed with a clean Vulkan teardown after the FSR3 compatibility guard. FSR3 reports unavailable and rendering falls back to TAA.

Windowed Cornell screenshots also passed on Vulkan and D3D12. The Vulkan run requested ray tracing and exercised the pre-present copy. These checks used the desktop Wayland surface; the full tour uses X11 under Xvfb so display scaling does not change its expected pixel dimensions.

The complete feature tour passed all 32 capture checks at 1280x720 with zero Vulkan validation errors. It exercised RT lighting, streamed geometry, animation, water, hair, TAA/MSAA, upscalers, fog, and reconstructed path tracing. The tour also repeatedly exercised windowed screenshot readback.

Logs, captures, image comparison measurements, and the local runner scripts are saved in `build/bugbash` (ignored build artifacts). Changes are left uncommitted.

## Limits and tradeoffs

- The documented `tests/golden/golden.py` harness and its references are absent from this checkout. Deterministic before/after captures and the existing feature tour provide the available visual checks.
- Native Windows DXR and ray tracing through vkd3d-proton were not executed. D3D12 compiled and its available rendering paths were exercised.
- The existing ARM NGX driver workaround remains. Its driver-specific shutdown behavior could not be retested on this x86 machine.
- The bundled FidelityFX SDK does not consistently unwind partial context creation. The reproduced unsupported-memory case is prevented before initialization; arbitrary SDK allocation failures still need an upstream transactional-cleanup fix.
- FP32 diffuse/specular history moments add 32 bytes per render pixel, about 63 MiB at 1920x1080. Allocation failures now disable the affected path safely.
- Async graphs without a valid join use serial execution. They must declare a join before presentation work to recover async overlap.
- Validation and these regressions cover the exercised paths and drivers. They do not prove every scene, device, or malformed asset is safe.


## Path-tracing follow-up

The subsequent code scan found four additional bugs: DI proposal weighting, sky-cell sampling density, textureless alpha-mask handling, and accumulation after live material edits. All four are fixed, with GPU regressions that reproduced the original failures. See [the path-tracing audit](PATH_TRACING_CODE_AUDIT.md) for details and remaining concerns.

The follow-up full build and all 140 CTest entries passed. The three new regressions also passed on llvmpipe, and sampling/alpha coverage executed on D3D12. Four additional reference, reconstruction, and NRD captures passed with no Vulkan validation errors. Follow-up artifacts are in `build/bugbash/path-fixes`.


A further estimator audit found and fixed zero-contribution samples being omitted from reservoir-reuse normalization in six DI/sky/GI temporal and spatial paths. A production-shader energy test returned 1.25 where the expected mean was 1; it now returns 1. The extended tests, full 140-entry suite, and four integration captures passed. See the path-tracing audit and `build/bugbash/reuse-fixes` for reproduction evidence. Successful API validation alone is not evidence of correct light transport.

## Motion and upscaler follow-up

The [motion/upscaler audit](MOTION_UPSCALER_AUDIT.md) fixes traced rigid/skinned motion, surface identity, geometry-change accumulation, temporal GI reconnection, dim-path continuation, sky jitter, SDK timing/history, and RR guides. Dedicated SDK captures also exposed that RR feature creation lacked the required input-resolution motion flag and that activating RR allocated unused graph textures with zero usage. Both are fixed. Numerical DLSS/FSR image tests, shared NGX lifetime/resize checks, the 143-entry suite, llvmpipe ray queries, and eight captures passed.

The final motion/upscaler pass also completed all 32 feature-tour capture checks with zero Vulkan validation or SDK evaluation errors. Artifacts are in `build/bugbash/motion-upscalers`.
