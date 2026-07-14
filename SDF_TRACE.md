# SDF visibility tracing — software ray path for RCGI

Investigation branch: can rx run RCGI's world side on hardware without ray
query, by tracing signed-distance fields instead of the TLAS (the Lumen
"software ray tracing" idea grafted onto the RCGI backbone)? Target
milestone: **Cornell bounce lighting with `--no-rt`** through the RCGI
probes-only tier, with an honest go/no-go assessment at the end.

Why this shape: RCGI's rays are visibility-only (zero shading at the hit —
see RCGI.md), so the tracer is an abstract "origin+dir -> hit pos/normal/
surface color" query. Swapping `RayQuery` for an SDF sphere-trace behind
that seam leaves the entire cache/cascade/composite pipeline untouched.
The final gather (M2) stays hardware-only for now; the software tier uses
the probes-only resolve (`RX_RCGI_PROBES_ONLY` path), which is exactly the
scalability story: same code path, lower dial.

## Milestones

- **S1 — SDF infrastructure** (`engine/render/gi/sdf_*`):
  mesh SDF generation at load, a camera-following global SDF clipmap with a
  surface-color proxy, and a raymarched debug visualization to verify all of
  it standalone (no RCGI coupling yet).
- **S2 — RCGI integration**: software trace variants of the RCGI world-side
  shaders behind a shared seam, cache entries that carry surface color
  instead of triangle ids, renderer gating so `--no-rt` + `RX_RCGI=1` works,
  GPU verification + measured costs + findings write-up.

## S1 design

### Mesh SDFs

- One distance volume per unique mesh (keyed like BLASes are, by mesh key).
  Resolution scales with mesh extent, clamped to 16..64 per axis; ~0.5-1
  voxel narrow-band accuracy is enough (visibility rays, not rendering).
  R16Float, tight local-space AABB with 1-voxel padding.
- Generation at mesh upload (prototype: acceptable to be CPU-side + slow-ish;
  cache-to-disk optional/later). Distance = exact point-triangle distance via
  a uniform triangle grid; sign via axis ray parity (odd/even crossings, 3
  axes majority vote). Open/thin meshes will leak — document, don't fight
  (Lumen has the same failure class).
- Per-mesh average surface albedo + emissive computed at load from material
  base_color/emissive factors (texture-average later; note the limitation).

### Global SDF clipmap

- 4 clips mirroring the RCGI cascades: clip 0 spans 32 m, doubling to 256 m,
  camera-snapped. 128^3 R16Float distance per clip (~17 MB total) plus a
  surface-color proxy per clip: RGBA8 albedo + RGBA8 emissive at the same res
  (~134 MB total at 128^3 — note the knob: 96^3/64^3 tiers cut this to
  ~57/17 MB; record actuals in the findings).
- Composition: compute pass per clip; each voxel min-blends the mesh SDFs of
  instances whose (conservatively scaled) bounds overlap it, transforming
  voxel centers into mesh local space (non-uniform scale: use max axis,
  conservative). Instance list = the same scene draw list the TLAS build
  consumes. Amortized like the cascades: one clip re-composited per frame,
  all clips on snap/teleport. Winner also writes its albedo/emissive into
  the color proxy.
- Debug view: full-screen raymarch of the clipmap (distance visualization +
  hit albedo), toggled by env (`RX_SDF_DEBUG=1/2`), verified against demo
  scenes before S2 starts.

### Trace

`TraceGlobalSdf(origin, dir, tmax)` in a shared .hlsli: sphere-trace the
finest clip containing the ray segment, stepping up clips as the ray exits;
hit when distance < surface epsilon (scaled by voxel size); normal from
6-tap central-difference gradient; returns hit position, normal, albedo,
emissive, hitT, miss flag. Self-hit avoidance: start bias of ~1 voxel along
the normal/direction.

## S2 design

- **Shader variants, not runtime branches**: a pipeline containing RayQuery
  can fail creation on non-RT devices, so build `_sw` compute variants of
  `rcgi_probe_trace` and `rcgi_cache_shade` that #include the same body with
  the trace seam swapped (follow the existing mesh.ps -> mesh_rt.ps variant
  generation pattern in the build).
- **Cache entries without triangle ids**: SDF hits have no instance/primitive
  to re-resolve. The M1 entry already stores world pos + oct normal
  (slots 4..7); the software variant stores packed albedo+emissive in the
  instance/primitive/barycentric slots (unused in sw mode) and the sw cache
  shade lights from those directly: sun (SDF shadow trace) + light grid +
  previous-frame cascade bounce + emissive. Sky miss path unchanged.
- **Renderer gating**: allow RcgiSystem creation without ray query when the
  SDF path is available; `rcgi_active` becomes
  `settings.rcgi && (rt || sdf_ready)`; software mode forces the probes-only
  resolve; TLAS build/binding skipped in sw mode. `RX_RCGI_SW=1` forces the
  software tracer on RT hardware for A/B comparison. Everything else
  (light grid, blend/border, composite, settings) is untouched.

## Verification

Real GPU (`vkrun`), as always:
- `--no-rt RX_RCGI=1 --demo cornell`: red/green bounce present, no crash,
  no validation errors. Compare against the hardware probes-only shot
  (`RX_RCGI_PROBES_ONLY=1`, RT on) — should be close, modulo SDF rounding.
- `RX_RCGI_SW=1` (RT hardware, software trace) A/B for image + perf deltas.
- `--demo materials`, sponza as a stretch (records mesh-SDF gen time for a
  real scene).
- Off / RT-path regression: `RX_RCGI=1` without sw flags must be
  screenshot-identical to before; RX_RCGI unset unchanged.

## Findings (S2 — measured on NVIDIA GB10, vkrun, 1920×1011)

**Headline: `--no-rt RX_RCGI=1 --demo cornell` works.** The device comes up
`rt=false rayquery=false`; RCGI's world side runs entirely through the SDF
clipmap tracer (the sw pipelines carry zero RayQuery capability in their SPIR-V,
verified via `spirv-dis`), and the classic red-left / green-right Cornell colour
bleed is present on the floor and both boxes. No crash, no NaN/speckle,
converged. `RX_DEBUG_VIEW=6` isolates the indirect channel and shows the bounce
clearly.

### Memory actuals

- **Global clipmap (constant, scene-independent, `kRes=128`)**: distance R16Float
  16 MiB + albedo RGBA8 32 MiB + emissive RGBA8 32 MiB = **~80 MiB**. Tier knobs:
  96³/clip ≈ 34 MiB, 64³/clip ≈ 10 MiB (one code constant).
- **Mesh SDFs (per-scene, float32 buffers)**: cornell 6 meshes **2.30 MB**;
  materials 27 meshes **6.75 MB**; sponza single 262k-tri mesh 84 KB (clamped res).

### Generation times (CPU, at load — from S1, re-confirmed)

- cornell 6 meshes **154 ms** total; materials 27 meshes **1.15 s** (the two
  1-box floor/wall meshes are cheap, the 3072-tri spheres ~30–40 ms each); sponza
  262k-tri mesh 238 ms. Well inside a load budget; not yet disk-cached.

### Compose + trace costs (cornell, `RX_GPU_TIMINGS=1`, steady state)

| Pass                                  | hw (TLAS, probes-only) | sw (SDF clipmap) |
|---------------------------------------|:----------------------:|:----------------:|
| `rcgi` world (trace+shade+blend+border) | **0.29 ms**            | **0.22 ms**      |
| `sdf_compose` (one clip / frame)      | — (not built)          | **0.25 ms**      |
| combined world side                   | **0.29 ms**            | **~0.47 ms**     |

Notable: the SDF sphere-trace of a coarse Cornell field is *cheaper* than TLAS
traversal + bindless normal fetch (0.22 vs 0.29 ms), but software additionally
pays the ~0.25 ms `sdf_compose` (one clip re-composited per frame) that hardware
avoids. `RX_RCGI_SW=1` on RT hardware measures identically to `--no-rt`
(rcgi 0.22, compose 0.26) — the tracer is the same, the TLAS is simply ignored.
Materials `sdf_compose` is the scaling concern (S1: 1.3–1.8 ms; whole-clip-per-
instance doesn't cull — see below).

### Image comparison

- **(a) hw probes-only vs (b) sw (both probes-only)**: same red/green bleed,
  same directional character. Software is slightly *stronger* and *blockier* —
  the clipmap voxelisation rounds the box silhouettes and the albedo proxy is a
  flat per-mesh colour, so the bleed reads as coarser bands. Exactly the "differ
  only by SDF geometric rounding" prediction.
- **(b) RX_RCGI_SW on RT vs (c) sw via --no-rt**: near-identical (pixel-level
  differences only from the RT-present frame timing, not the GI). Confirms the
  software path is self-contained and TLAS-independent.
- **materials --no-rt**: plausible ambient across the sphere grid, no crash, no
  speckle. Albedo is per-mesh-averaged (flat), so material variety is muted vs a
  texture-aware gather.
- **Whole-stack `--no-rt` vs RT (post-rebase, quantified)**: full-frame RMSE
  between the RT final (M2 gather + all RT features) and the `--no-rt` software
  final is **2.2 %** on cornell and **8.7 %** on sponza — the sponza delta is
  dominated by the RT-gated volumetric-fog haze (absent under `--no-rt` by
  design), not by the GI: bounce lighting, banner colour response and vault
  shading match. Indirect-channel RMSE across the A/B trio (hw probes-only vs
  `RX_RCGI_SW` vs `--no-rt`) is ~3.3 %, the predicted voxel-rounding delta.
  Frame-to-frame RMSE at steady state is ~1.6 % (animated clouds + moving sun),
  with no speckle or divergence — the software tier converges temporally.

### Artifact / leak inventory

- **Texture-averaged (really factor-averaged) albedo**: the colour proxy is the
  material `base_color_factor`, not a texture average — textured surfaces bleed a
  flat colour. Most visible in materials/sponza; invisible in the flat-coloured
  Cornell.
- **Voxel rounding**: coarse clips round corners and inflate thin silhouettes;
  the bleed bands are blockier than the hardware triangle hit. Expected.
- **Binary SDF sun shadow**: the sw cache shade traces one occlusion ray against
  the clipmap (hit ⇒ fully shadowed). No penumbra; coarse-clip self-occlusion is
  mitigated by a **clip-scaled** start bias (normal offset + trace initial step
  both scaled by the hit clip's voxel — see the PR #13 review-fix section) but can
  still over-darken near thin geometry.
- **Cornell demo walls render translucent** in every mode (hw and sw alike) —
  that is the demo's wall material, not an S2 artifact.
- **Thin/open geometry SDF sign leaks** (S1's ray-parity failure class) and the
  single-mesh-sponza blobbing are unchanged from S1.

### Go / no-go recommendation

**GO for a low-tier / non-ray-query GI fallback** — the milestone (Cornell bounce
with `--no-rt`) is met, the world side is trace-agnostic behind a clean seam, and
the sw cost is competitive. It is a *probes-only* tier: the M2 final gather stays
hardware-only. First fixes, in priority order, before this is production-grade:

1. **Software final gather.** Replace the M2 gather's `RayQuery` with a
   `TraceGlobalSdf` gather ray + the existing screen-cache / world-cache /
   irradiance three-level fallback. This is the single biggest quality lever
   (per-pixel contact GI vs the flat probes-only resolve) and the seam already
   exists — it is the obvious S3.
2. **Scalable clipmap compose.** Whole-clip-per-instance is O(instances·128³)
   and does not cull (materials 1.3–1.8 ms, sponza worse). A bindless mesh-SDF
   atlas + single dispatch, or a jump-flood, is the replacement.
3. **Texture-averaged albedo.** Bake a per-mesh average from the base-colour
   texture (not just the factor) so bleed carries real surface colour.
4. **Disk-cache mesh SDFs.** Materials' 1.15 s load-time gen should be cached
   (keyed by mesh hash) so it is paid once.
5. **Softer sun occlusion.** Distance-attenuated (cone/penumbra) SDF shadow
   instead of binary, and an emissive HDR scale (the proxy is RGBA8 LDR, so
   emissive magnitude is clamped to 0..1 today).

---

## S1 implementation notes (landed, GPU-verified on NVIDIA GB10 / vkrun)

S1 is implemented and verified standalone (no RCGI coupling). `RX_SDF=1` builds
per-mesh SDFs at upload and composes a camera-following global SDF clipmap with a
surface-colour proxy every frame; `RX_SDF_DEBUG=1|2` raymarches it onto the scene
colour. Off by default (`RenderSettings::sdf`), zero behaviour change when unset.

### Source map

- `engine/render/gi/sdf_scene.{h,cc}` (`class SdfScene`) — per-mesh SDF generation
  + registry, keyed by the same `u64` mesh key as the BLAS path.
- `engine/render/gi/sdf_clipmap.{h,cc}` (`class SdfClipmap`) — the 4-clip volumes,
  clear/compose/debug passes, snapping, globals UBO, and the S2-facing accessors.
- `engine/render/shaders/gi/sdf_trace.hlsli` — `TraceGlobalSdf` + `SdfGlobals` +
  clip sampling helpers (the S2 seam; dependency-free, no RayQuery/bindless).
- `engine/render/shaders/gi/sdf_clear.cs.hlsl`, `sdf_compose.cs.hlsl`,
  `sdf_debug.cs.hlsl`.
- Wiring: `core/renderer.{h,cc}` (members `sdf_scene_`/`sdf_clipmap_`, `RX_SDF`/
  `RX_SDF_DEBUG` options, seed `settings_.sdf`, `RegisterMesh` hook in `UploadMesh`,
  compose pass after `tlas_build`, debug pass after `debug_lines`, teardown in
  `Shutdown`), `core/settings.h` (`sdf`), `app/host.cc` (carry `RX_SDF` through the
  preset — this is mandatory; `ApplyRenderPreset` overwrites `settings_` wholesale),
  `pipeline/material_system.{h,cc}` (`material_color()` + a CPU factor map).

### Mesh SDFs (`SdfScene`)

- Generated CPU-side at `Renderer::UploadMesh` (opaque meshes only), from the lod-0
  vertex positions + indices. Resolution per axis `clamp(ceil(ext/voxel)+2·pad, 16,
  64)`, cubic voxel `≈ maxext/36`, voxel grown if any axis would clamp past 64 so the
  volume always covers the mesh + 2-voxel padding; volume centred on the mesh AABB.
- Distance = exact point–triangle distance (Ericson closest-point) accelerated by a
  uniform triangle grid (~3 voxels/cell, CSR lists) with a ring-expanding closest
  search. Sign = 3-axis ray-parity majority vote; an axis-aligned ray stays in one
  cell row, so parity is O(row·tris/cell) with cell-ownership dedup. Parallelised
  over z-slices via `core/job_system`.
- **Storage: a flat `StructuredBuffer<float>` (one signed float/voxel), NOT a 3D
  texture.** The RHI's `CopyBufferToTexture` is 2D-only (hardcoded depth 1), so a 3D
  mesh-SDF upload would need a per-mesh compute copy; a flat buffer sampled with
  manual trilinear in the compose shader is simpler and sidesteps that. f16 packing
  is a future 2× memory win (deferred).
- Per-mesh average albedo + emissive from material `base_color_factor` /
  `emissive_factor` only (via the new `MaterialSystem::material_color`), weighted by
  submesh index count. **Texture averaging is out of scope** (noted limitation).

### Global SDF clipmap (`SdfClipmap`)

- 4 clips, extents 32 / 64 / 128 / 256 m, per-axis resolution `kRes = 128` (a single
  code constant — lower it for tiers). Camera-snapped per clip (`floor((cam-half)/
  voxel)*voxel`), one clip recomposited per frame round-robin plus any clip that
  snapped this frame; all four on the first frame.
- **One stacked-Z atlas per channel** of size `(128, 128, 128·4=512)`: clip `c` owns
  z in `[c·128, (c+1)·128)`. Three volumes:
  - `distance` — **R16Float** 3D storage image.
  - `albedo`  — **RGBA8Unorm** 3D storage image (surface albedo proxy).
  - `emissive`— **RGBA8Unorm** 3D storage image (surface emissive proxy; 0..1, so
    HDR emissive magnitude is clamped — S2 can reintroduce a material emissive scale).
  Both `R16Float` and `RGBA8Unorm` 3D **storage** images validate clean on the GB10
  (the froxel fog only ever used RGBA16F 3D, so this is newly exercised; no format
  or barrier validation errors from the SDF passes on or off).
- Volumes live permanently in `kGeneral` (primed once via `ImmediateSubmit`),
  hand-barriered like `RcgiSystem` (`MemoryBarrier(kComputeRead→kComputeWrite)` at
  pass start, `kComputeWrite→kComputeRead` between dispatches and trailing).
- **Composition** (`sdf_clear.cs` then `sdf_compose.cs`): per recomposed clip, clear
  the slab to `kFarDistance=1e4` + zero colour, then **one whole-clip dispatch per
  overlapping instance** that min-blends its conservative distance; the winner writes
  albedo/emissive. Instance list = the scene draw list (mirrors the TLAS gather:
  mesh key + transform; colour comes from the mesh's `MeshSdf`).
  - Voxel centre → mesh local via the inverse instance transform; inside the mesh
    volume box → trilinear SDF sample, outside → distance-to-box + clamped edge value
    (monotone outward, no false shell at the box). Local distance → world via a single
    conservative scale factor.
  - **Non-uniform scale: I use the transform's *minimum* axis scale, not the max the
    design doc names.** Min scale is the correct conservative (never-overshoot) lower
    bound on world distance for a sphere trace; max scale can make the trace step
    through geometry. Error: on strongly non-uniform scale the field is shrunk toward
    the compressed axis (over-conservative, slower marching), never inflated.
  - **Scaling limit:** whole-clip-per-instance is O(instances · 128³). Correct and
    dense (every voxel gets a valid conservative distance, so tracing never
    overshoots), but it does not cull. A bindless mesh-SDF-atlas + single dispatch, or
    a jump-flood, is the scalable replacement — documented, not built for S1.
  - Empty space stays `kFarDistance` (cleared every recomposition; never garbage).

### Trace seam (`sdf_trace.hlsli`) — the S2 interface

```
SdfHit TraceGlobalSdf(float3 origin, float3 dir, float tmax,
                      SdfGlobals sdf,
                      Texture3D<float>  sdf_distance,
                      Texture3D<float4> sdf_albedo,
                      Texture3D<float4> sdf_emissive,
                      SamplerState      sdf_sampler);
// SdfHit { float3 pos, normal, albedo, emissive; float hitT; bool miss; }
```

Sphere-traces the finest clip containing the ray, stepping up clips on exit; hit when
distance < 0.75·voxel; 6-tap central-difference gradient normal; ~1-voxel start bias;
step floored to `0.5·eps` for progress. Clip sampling clamps to the clip's z-slab to
avoid cross-clip bleed. **S2 must bind (any slots, passed as params):**

- `ConstantBuffer<SdfGlobals>` — get it from `SdfClipmap::globals(frame_index)`
  (host-visible, ping-pong by frame parity; layout: `float4 clip_origin[4]`
  {xyz world-min, w voxel}, `float4 clip_params` {res, clips, total_z, far},
  `float4 camera_pos`).
- `sdf_distance` = `SdfClipmap::distance_volume()` (R16Float), `sdf_albedo` /
  `sdf_emissive` = the RGBA8 proxies — **bind `InGeneral(...)`** (they live in
  kGeneral), sampled with `SdfClipmap::sampler()` (linear clamp-to-edge, all axes).

Reconstruct the ray with the near-plane point minus the camera:
`dir = normalize((inv_view_proj·(ndc,1,1)).xyz/w − camera)`. **Do not** use the far
plane (reversed-Z depth 0 = infinite far ⇒ divide-by-≈0 ⇒ NaN direction). This was the
one non-obvious trap while bringing the debug view up.

### Debug view (`sdf_debug.cs`, `RX_SDF_DEBUG`)

Late full-screen compute writing straight onto `lit` (mirrors `froxel_apply`).
`1` = distance field (tint by clip + gradient shading + ~1 m distance bands);
`2` = hit albedo + gradient-normal shading (a crude SDF view of the scene — the S1
acceptance test). Primary standalone verification tool.

### Measured (GB10, vkrun, 1920×1011)

- **Mesh-SDF gen** (CPU, at load): cornell 6 meshes 169 ms total (2.3 MB); materials
  27 meshes 1.22 s (6.75 MB); **sponza a single 262 267-tri mesh in 238 ms** (grid
  accelerator; 84 KB at the clamped 40×20×27 res). Well inside the "tens of seconds"
  budget.
- **Composition** (one clip/frame): cornell (6 instances) **0.48–0.54 ms** (under the
  ~1 ms target); materials (27 instances) **1.3–1.8 ms** (over target — the
  whole-clip-per-instance scaling limit above). `sdf_debug` full-screen raymarch
  **~0.14 ms**.
- **Memory** at `kRes=128`: distance 16 MiB + albedo 32 MiB + emissive 32 MiB =
  **~80 MiB** clipmap, constant regardless of scene. Knob: 96³/clip ≈ 34 MiB,
  64³/clip ≈ 10 MiB. Mesh SDFs are float32 buffers (per-scene, see gen figures).

### Verification evidence

- `RX_SDF_DEBUG=2` renders a recognizable cornell (red/green walls, floor, both
  boxes, correct per-mesh albedo) and materials (sphere grid with red/white/blue rows)
  built purely from the clipmap; sponza resolves floor/columns coarsely.
- `RX_SDF` unset: **bit-identical** to HEAD (`RX_FIXED_DT` diff, max 0). `RX_SDF=1`
  without debug: **bit-identical** to off (compose runs, scene colour untouched).
- `--validation` on+off: **no new errors/warnings from the SDF passes** (diff of
  validation signatures vs baseline is empty; the pre-existing `caustic_out`/`cur_ring`
  storage-format warnings and the unclean-screenshot-exit buffer-leak reports are HEAD
  behaviour, present with SDF off too).
- RCGI untouched: `RX_RCGI=1` alone is unaffected (RX_SDF adds nothing when unset);
  `RX_RCGI=1 RX_SDF=1` differs by max 5/255 (compose pass nudges RCGI's async/temporal
  timing only; RCGI code unchanged).

### Artifact / leak inventory

- **Thin/open geometry**: ray-parity sign leaks on non-watertight meshes (Lumen's
  failure class); rounded/inflated silhouettes on coarse clips are expected.
- **Coarse single-mesh scenes**: sponza-as-one-mesh clamps to 64³ ⇒ 0.83 m voxels ⇒
  columns/arches blob together. A real pipeline splits large meshes or builds a
  scene-wide SDF; out of scope for S1.
- **Non-uniform / degenerate scale**: min-axis scale (see above); a zero-thickness
  scaled instance has a non-invertible transform (`Inverse` returns identity) and
  would smear — the demo content is translation-only boxes/spheres so this is untested.
- **No true resource leak**: `SdfScene`/`SdfClipmap` free their buffers/images/
  pipelines in `Renderer::Shutdown` (the validation leak reports are the pre-existing
  abrupt-screenshot-exit path, identical count with SDF off).

---

## S2 implementation notes (landed, GPU-verified on NVIDIA GB10 / vkrun)

S2 wires the S1 clipmap tracer into RCGI so the world side runs without hardware
ray query. `--no-rt RX_RCGI=1` brings up the SDF clipmap and traces it in place
of the TLAS; `RX_RCGI_SW=1` forces the same path on RT hardware for A/B.

### Variant / seam mechanics

- The two RayQuery-using world shaders were refactored into shared bodies with a
  compile-time trace seam (`RCGI_TRACE_SDF`), following the mesh.ps/mesh_sss.ps
  thin-entry pattern:
  - `shaders/gi/rcgi_probe_trace_body.hlsli` + thin entries
    `rcgi_probe_trace.cs.hlsl` (hw, `RCGI_TRACE_SDF` undefined) and
    `rcgi_probe_trace_sw.cs.hlsl` (`#define RCGI_TRACE_SDF 1`).
  - `shaders/gi/rcgi_cache_shade_body.hlsli` + `rcgi_cache_shade.cs.hlsl` /
    `rcgi_cache_shade_sw.cs.hlsl`.
- The `#ifndef RCGI_TRACE_SDF` guards drop the `RaytracingAccelerationStructure`
  binding, the bindless scene tables, and every `RayQuery` in the software build,
  so the sw SPIR-V declares **no RayQuery/accel-struct capability** (confirmed:
  `spirv-dis rcgi_probe_trace_sw.cs.hlsl.spv | grep -c RayQuery` == 0, vs 12 for
  the hw variant). This is why the sw pipeline creates on `rayquery=false`
  devices where a RayQuery module would fail pipeline/device creation.
- Hardware variants are behaviourally unchanged: the hw path is the same code
  (the `RcgiClaimCell` / `RcgiQueueIfStale` helpers are verbatim extractions of
  the original inline cache-insert). RT regression verified (hw probes-only
  Cornell bleed intact).
- The clipmap tracer itself is the S1 seam `shaders/gi/sdf_trace.hlsli`
  (`TraceGlobalSdf`), unchanged and shared by both `sdf_debug` and the two sw
  variants. SDF resources bind as **set 1**: `{0 SdfGlobals UBO, 1..3
  distance/albedo/emissive Texture3D (InGeneral), 4 sampler}` — mirrors
  sdf_debug's wiring.

### Cache-entry packing (software mode)

An SDF hit has no instance/primitive/barycentric to re-resolve, so the sw probe
trace repurposes the triangle-ref slots (documented next to the layout in
`rcgi_common.hlsli`):

- slot 1 (`kRcgiOffHit0`) = albedo, 8-bit RGB in bits 0..23 (`RcgiPackColor8`).
- slot 2 (`kRcgiOffHit1`) = emissive, 8-bit RGB in bits 0..23, `| kRcgiSwEntryBit`
  (bit 31) tagging a software entry.
- slot 3 (`kRcgiOffHit2`) = free (0).
- slots 4..8 (world pos, oct normal, hitT) and 9/10 (frame stamps) keep their
  hardware meaning.

`rcgi_cache_shade_sw` reads albedo/emissive straight back from slots 1/2 instead
of the bindless material path, then lights: sun (a **binary** `TraceGlobalSdf`
occlusion ray toward the sun, ~1.5-voxel start bias, tmax = coarsest-clip extent)
+ light-grid point/spot loop (identical, no ray query) + previous-frame cascade
bounce (identical) + emissive. Same outputs (radiance buffer + stamps).

### Gating logic (renderer.cc + rcgi.{h,cc})

- `RcgiSystem::Create(..., bool rt_available)`: `CreatePipelines(rt_available)`
  always builds the sw pair + the shared/probes-only pipelines (args, blend,
  border, resolve); it builds the hw RayQuery pair (probe trace, cache shade) and
  the M2 gather chain (gather/denoise/upscale/history) **only when
  `rt_available`**. On RT hardware both pairs exist so `RX_RCGI_SW` A/B is free.
- RCGI is created **lazily** on first activation (in `ApplySettings`, not
  `Initialize` — a device that never enables it pays nothing, and creation
  failure is non-fatal). The lazy trigger is `settings_.rcgi && (rt_available_ ||
  (settings_.sdf && sdf_clipmap_)) && bindless_ && environment_ && !rcgi_ &&
  !rcgi_create_failed_`, so it fires for both the RT and the software-only path
  once the SDF clipmap is up. (This subsumes the merge with PR #12's review-fix
  lazy-init: the sw variant is lazy + non-fatal too.)
- Active predicate: `rcgi_active = rcgi_ && settings_.rcgi && settings_.ibl &&
  bindless_ && (rt_available_ || sdf_ready)`. New state
  `rcgi_software = rcgi_active && sdf_ready && (!rt_available_ ||
  rcgi_force_software_)`, and `rcgi_probes_only = RX_RCGI_PROBES_ONLY ||
  rcgi_software`.
- **SDF auto-enable**: in `Initialize`, `if ((settings_.rcgi && !rt_available_) ||
  rcgi_force_software_) settings_.sdf = true;` — so `--no-rt RX_RCGI=1` (and
  `RX_RCGI_SW` alone on RT) seeds the clipmap (RCGI-software implies the SDF cost).
  SDF availability is a **startup** decision — see the PR #13 review-fix section
  (finding 2) for the contract and why late/programmatic enables cannot backfill.
  Carried through the preset by the
  existing `tuned.sdf = env.sdf` / `tuned.rcgi = env.rcgi` lines in
  host.cc::ApplyRenderPreset (mandatory; the preset overwrites settings_
  wholesale). `rcgi_force_software_` is a renderer member (not a settings field),
  so it survives the preset independently.
- **Compose ordering**: the `sdf_compose` pass is now recorded *before* the RCGI
  world pass (S1 anchored it after `tlas_build`, which does not run in software
  mode). Both use the kGeneral manual-barrier discipline on the main timeline, so
  submission order is execution order; the compose's trailing
  `kComputeWrite→kComputeRead` barrier makes the volumes visible to the sw trace.
- **What software mode turns off**: `rcgi_async` is forced false (main timeline);
  the resolve is forced to the probes-only path (`AddResolvePass`, not the
  RayQuery `AddGatherChain`); `AddHistoryCopy` is skipped (only the gather chain
  consumes it); `tlas_shaded` excludes `rcgi_software`. Every other RT pass
  (rt shadows/AO/reflections, fog, the M2 gather) is already `rt_available_`-gated
  by `--no-rt`, so nothing extra to guard.
- `AddToGraph` gained a `const SdfClipmap* sdf` parameter (non-null ⇒ software)
  and takes `RayTracingContext*` (nullable) instead of a reference; the pass
  lambda binds the sw pipelines + SDF set when `sdf != nullptr`.

### Env matrix

- `RX_RCGI=1` — enable RCGI. On RT: hardware TLAS path (M2 gather). On non-RT:
  auto-enables `RX_SDF` and runs the software tracer (probes-only resolve).
- `RX_RCGI_SW=1` — force the software tracer even on RT hardware (A/B). Auto-
  enables `RX_SDF`. Same frame as hw except the tracer; resolve forced to
  probes-only.
- `RX_RCGI_PROBES_ONLY=1` — M1 per-pixel cascade resolve instead of the M2
  gather. Implied by software mode. Orthogonal to `RX_RCGI_SW` on hw (hw
  probes-only is the A reference).
- `RX_SDF=1` / `RX_SDF_DEBUG=1|2` — the standalone S1 path (clipmap build +
  raymarch debug), unchanged. `RX_SDF` is auto-set when RCGI-software needs it,
  but can still be set explicitly for the debug view.

## Rebase onto merged RCGI (PR #12) + review-fix port

This branch was rebased onto `main` after RCGI merged (commits c0d9521..eb15aaa,
the last being the RCGI review fixes: shutdown order, TLAS gating, cache atomics,
barrier scopes, cascade reset, eviction, lazy init). The S2 commit had extracted
the two world-side shaders into thin hw/`_sw` entry files plus shared
`rcgi_probe_trace_body.hlsli` / `rcgi_cache_shade_body.hlsli` (compile switch
`RCGI_TRACE_SDF`) from the **pre-fix** originals, so the review-fix behaviours had
to be ported into the shared bodies — covering the software path too — rather than
left behind in the (now deleted) monolithic shaders.

What landed in the shared bodies (identical for hw and sw unless noted):

- **Stamp encoding**: frame stamps are `RcgiStampEncode(frame)` = frame+1 (0 =
  never / cleared slot). `RcgiClaimCell` takes the encoded stamp.
- **Age eviction** (`RcgiClaimCell`): a probed slot owned by a different cell with
  a queued stamp older than `kRcgiEvictAge` (256) is reclaimed via a second
  key `InterlockedCompareExchange`; the winner zeroes the shaded stamp (slot 9)
  before use so the previous occupant's radiance cannot leak through
  `RcgiCacheLookup`.
- **Single-owner payload write**: after claiming the slot, one
  `InterlockedExchange` on the queued stamp (slot 10) elects the per-cell,
  per-frame owner; losers return without writing. **This now gates the software
  payload too** (albedo slot 1, emissive|`kRcgiSwEntryBit` slot 2, pos/normal) —
  the pre-fix sw path wrote those every ray, with the same tearing exposure the
  fix closed for the hw triangle refs. Only the triangle-ref slots differ between
  variants; pos/normal/hitT and the active-list append are shared.
- **Owner-only active-list append** (`RcgiAppendIfStale`), gated on staleness
  (shaded stamp 0 or age ≥ 4).
- **cache_shade**: hw branch gains defensive `GetDimensions` bounds checks on the
  instance / geometry-slot / material index before the bindless fetches. The sw
  branch does **no** bindless fetches (surface colour comes from the packed
  entry), so there is nothing to bound there. The shaded-stamp write uses the +1
  encoding.

**Entry packing is unchanged by the port.** The sw entry still uses slots 1/2/3
for albedo / emissive+`kRcgiSwEntryBit` / free; the fixes only touch slots 9/10
(stamps) and the active list, so `kRcgiSwEntryBit` (bit 31 of slot 2) does not
collide with anything the review fix added.

**Behavioural equivalence with main's fixed shaders** was established by direct
line-by-line reading: the hw entry+body expands to the same claim loop (with
eviction), single-owner `InterlockedExchange`, payload writes, and stale append,
in the same order, as `origin/main`'s fixed `rcgi_probe_trace.cs.hlsl` /
`rcgi_cache_shade.cs.hlsl`. The only deltas are the extraction into
`RcgiClaimCell` / `RcgiAppendIfStale` helpers, the `RCGI_TRACE_SDF` seam, and
hoisting `stamp`/`pos`/`n` above the seam — all side-effect-free, so no behavioural
change vs the merged fixes. GPU A/B (hw probes-only vs `RX_RCGI_SW` vs `--no-rt`)
renders equivalent bounce with no speckle.

## PR #13 review fixes (correctness, GPU-verified on NVIDIA GB10 / vkrun)

Eight review findings addressed. The hardware RCGI path is untouched: every
shared-body edit lives inside the `#ifdef RCGI_TRACE_SDF` seam, so the hw
`rcgi_probe_trace` / `rcgi_cache_shade` SPIR-V is **byte-identical** before/after
(verified by sha256 of the compiled `.spv` and a `dxc -P` preprocess diff of the
hw entry points — the only difference is shifted `#line` markers, which do not
affect codegen).

1. **Optional SDF pipelines are created outside the pipeline batch.** Inside a
   batch, `Create*Pipeline` returns a placeholder handle that only fails at
   `EndPipelineBatch`, so a failed SDF pipeline aborted the whole renderer instead
   of degrading. `SdfScene`/`SdfClipmap` are now built **after** `pipeline_batch.
   End()` in `Renderer::Initialize`, so a pipeline (or 3D-storage-image) failure
   surfaces at the call site and is handled non-fatally (log, tear down, leave the
   path unavailable). `RcgiSystem`'s `_sw` pipelines were already outside any batch
   (created lazily in `ApplySettings` during `RenderFrame`), so a null system there
   is likewise non-fatal.

2. **SDF availability is an explicit startup decision (option b).** The CPU mesh
   positions/indices used to voxelise mesh SDFs are not retained after upload
   (`GpuMesh` holds only GPU buffers), so there is no way to backfill the field on a
   late false→true `rcgi` toggle without retaining large CPU copies. Contract:
   set `RX_RCGI` (non-RT) / `RX_RCGI_SW` (RT A/B) / `RX_SDF` **at startup** to get
   the software path; `Initialize` seeds it via `if ((settings_.rcgi &&
   !rt_available_) || rcgi_force_software_) settings_.sdf = true;` (RX_RCGI_SW alone
   now seeds it too, independent of a later rcgi enable). A late programmatic rcgi
   enable on a non-RT device that seeded no SDF path is refused with a one-shot
   `RX_WARN` in `ApplySettings` rather than silently doing nothing; the debug-UI
   rcgi toggle is already greyed out when the device lacks ray query. RT hardware
   live toggles (hardware path) and `RX_RCGI_SW` at startup are unaffected.

3. **The SDF visibility set mirrors the realtime TLAS set.** `Renderer::UploadMesh`
   now skips `gpu.no_rt` meshes entirely (never in the realtime tlas — foliage,
   `exclude_from_rt` skinned actors) and builds the field from **only the opaque
   submesh index ranges** (blended submeshes — glass/water/effects — are excluded,
   so software RCGI no longer makes them occlude like opaque). The per-mesh average
   albedo/emissive is averaged over the opaque submeshes only. (Verified: the
   locomotion demo's `exclude_from_rt` biped is no longer registered as a bind-pose
   SDF; cornell/materials mesh counts are unchanged because those are opaque.)

4. **`sdf_compose` off-volume distance is a proven lower bound.** The previous
   `box_dist + SampleMesh(clamped)` was an upper bound and could let the tracer step
   through geometry. Replaced with `max(box_dist, SampleMesh(q) - box_dist)`, the
   max of two lower bounds: (a) the surface lies inside the mesh AABB so `box_dist ≤`
   distance-to-surface; (b) the 1-Lipschitz property `d(p) ≥ d(q) - |p-q|` with
   `|p-q| == box_dist` for an AABB clamp. Strictly conservative — may legitimately
   *tighten* geometry (fewer leaks).

5. **Rays starting just outside the guarded clip volume re-enter instead of
   missing.** An outer RCGI probe can sit on the coarsest clip boundary, and the
   small fixed initial step cannot bridge the ~2 m coarse-voxel gap to the 1-voxel
   guard band, so every direction read as a miss and injected sky into that cascade.
   `TraceGlobalSdf` now slab-tests the coarsest guarded AABB when the origin selects
   no clip and advances `t` to the entry point (clips are nested, so a mid-trace exit
   past the coarsest bound is still final).

6. **Inside/backface behaviour matches the hardware path.** A negative SDF sample at
   the ray start (ray began inside closed geometry) now returns a distinct `inside`
   status from `TraceGlobalSdf` instead of a spurious positive-distance hit; the sw
   probe trace maps it to the hardware backface case (rays-buffer `w == 0`, **no**
   cache insertion), which the blend pass reads as a visibility-only shortening ray.

7. **Sun-shadow bias scales with the hit's clip.** Both the normal offset and the
   trace's initial step previously used clip 0's 0.25 m voxel; a clip-3 surface
   (1.5 m hit epsilon) then self-intersected and set `shadow = 0`, dropping direct
   sun from coarse-cascade cache entries. The sw sun-occlusion trace now selects the
   clip containing the hit and scales **one coherent bias** (normal offset + the
   trace's `start_t` initial step) by that clip's voxel size.

8. **Same-key mesh re-upload regenerates the SDF.** `SdfScene::RegisterMesh` no
   longer early-returns on an existing key (which permanently pinned the first,
   now-stale SDF). It destroys the previous buffer (accounting the memory) and
   regenerates from the new geometry, mirroring `UploadMesh`'s WaitIdle-then-destroy
   discipline (registration is load-time, never per frame).

### Verification (GB10, vkrun, 1920×1011)

- Build green; `ctest` 13/13.
- `--no-rt RX_RCGI=1 --demo cornell`: red/green bounce present, converged; the
  indirect channel (`RX_DEBUG_VIEW=6`) shows the bleed with no sky poisoning.
- `RX_RCGI=1 RX_RCGI_SW=1` (RT) vs `RX_RCGI_PROBES_ONLY=1` (RT): equivalent modulo
  voxel rounding (sw slightly stronger/blockier, as before); the fix-4 lower bound
  visibly tightens the near-box shadow (fewer leaks).
- `--no-rt RX_RCGI=1 --demo materials` and Sponza `--gltf`: whole stack renders,
  bounce present, no crash/speckle.
- `RX_RCGI=1` (hw path): renders the full M2 gather; hw SPIR-V byte-identical to
  pre-fix (see above).
- `RX_RCGI` unset: zero SDF allocations. `RX_RCGI_SW=1` alone on RT: SDF seeded at
  startup.
- `--validation` sw + off: only the pre-existing texture-format warning; hw: only
  the known pre-existing async `rcgi_args` indirect-barrier error. No new errors.
