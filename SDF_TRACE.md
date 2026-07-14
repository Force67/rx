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

## Findings (fill in at the end)

- Memory actuals per tier, SDF gen times, composition + trace costs.
- Image comparison sw vs hw probes-only.
- Leak/artifact inventory (thin walls, open meshes, non-uniform scale).
- Go/no-go recommendation for productionizing (incl. what a software final
  gather would additionally need).

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
