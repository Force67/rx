# RCGI — Radiance-Cached Global Illumination (idTech8-style)

Optional dynamic-GI mode modeled on id Software's idTech 8 GI
("FAST AS HELL: idTech 8 Global Illumination", Tiago Sousa, SIGGRAPH 2025
Advances in Real-Time Rendering). Replaces the single DDGI volume + SSGI
indirect-diffuse path when enabled. Off by default: `RenderSettings::rcgi`,
env `RX_RCGI=1`, debug-overlay toggle.

Frame pipeline (all compute, async-capable like DDGI):

```
Update Light Grid -> Sample World Visibility -> World Radiance Cache ->
Irradiance Volumes -> Final Gather -> Denoise -> Upscale/Composite
```

## Components

### 1. Cascaded light grid (`gi/light_grid.{h,cc}`, `shaders/gi/light_grid.cs.hlsl`)

World-space clustered light binning so ray hits *outside the frustum* can be
lit (the existing froxel `light_cluster.cs.hlsl` is frustum-tied).

- 16x16x16 cells x 4 cascades, exponential bounds: cascade 0 = 32 m across
  (2 m cells), each further cascade 2x the extent. Camera-snapped to cell
  size (like DDGI origin snapping).
- Bins the frame's dynamic lights (`Light` struct from
  `light_cluster.cs.hlsl`; 256 max) — no decals/probes in v1.
- Max 32 light ids per cell. Output: per-cell count + id list buffers.
- Single dispatch, 4x4x4 thread groups, hierarchical gather: coarse cull to a
  groupshared flat bit array (256 lights = 8 uints) with sphere-vs-AABB of
  the *group's* bounds, then per-thread detailed cell test.
- Consumed by the radiance-cache shade pass (and available to future
  world-space consumers).

### 2. World radiance cache (`gi/rcgi.{h,cc}`, `shaders/gi/rcgi_*.hlsl`)

Spatially hashed radiance cache over ray-hit points [Gautron2020].

- 1D hash table, `kCells = 1<<20` entries. Base cell 25 cm, distance LOD:
  `lod = exp2(floor(log2(1 + dist/lodDist)))`, `cellSize = 0.25 * lod`.
  Hash of (quantized pos, lod) with linear probing (max ~8 steps; drop on
  overflow).
- Per entry: key/checksum (u32), packed hit (instance+primitive+barycentrics
  +hitT, 128 bit), radiance (RGBA16F or R32-packed RGB9E5), last-used frame
  stamp for eviction/reuse (entries re-shaded round-robin, reused N frames).
- **Probe trace pass**: for the current frame's irradiance cascade, each
  probe traces N visibility rays (fibonacci sphere + per-frame rotation, as
  DDGI does; 32/frame default). Store per-ray: hitT into the ray buffer AND
  register the hit cell in the hash (atomic append to an active-cell list).
  Miss = sky.
- **Cache shade pass**: one indirect dispatch over the active list. Fetch
  triangle/material through the bindless scene tables (same path as
  `ddgi_rays`), light with: sun (shadow ray) + emissive + point/spot lights
  from the **light grid** + previous-frame irradiance cascades (multi-bounce).
  rx is bindless end-to-end, so the spec's per-SBT-shader dispatch collapses
  to a single dispatch.

### 3. Cascaded irradiance volumes (in `gi/rcgi.{h,cc}`)

DDGI-style octahedral probe cascades fed from the radiance cache.

- 16x16x16 probes x 4 cascades, exponential spacing matched to the light
  grid (cascade 0 spacing 2 m). Camera-following, snapped, hysteresis
  re-convergence after snaps (reuse `DdgiSystem` blend logic).
- Interleaved updates: 1 cascade per frame, round-robin.
- Atlas: 8x8 irradiance texels + 8x8 RG16F visibility (mean/mean^2 hit
  distance), 10x10 with border. Irradiance RGBA16F v1 (RGB9E5 storage-image
  support is spotty). Blend + border passes mirror `ddgi_blend`/`ddgi_border`.
- Sample API in `rcgi_common.hlsli`: trilinear 8-probe with chebyshev
  visibility weights + cascade selection/blend, callable from any pass.

### 4. Final gather (`shaders/gi/rcgi_gather.cs.hlsl`)

- Half-res (quality option: quarter). 1 cosine-weighted ray per pixel around
  the g-buffer normal, blue-noise/IGN direction variation per frame.
- Zero shading; three-level cache fallback:
  1. **Screen cache**: hit point inside the previous frame's frustum,
     unoccluded vs prev depth -> previous frame's lit HDR color.
  2. **World radiance cache**: hash lookup at hit point (try lod, lod+1).
  3. **Irradiance cascades** at the hit point (or sky on ray miss).
- Accumulate into 2-band SH per pixel: 3x RGBA16F (R,G,B channel SH vectors;
  each pixel is an irradiance probe). Also write hitT for denoise weights.

### 5. Denoise + upscale (`rcgi_denoise.cs.hlsl`, `rcgi_upscale.cs.hlsl`)

- 2x separable "bilateral" gaussian at gather res on the SH triple,
  weighted by depth/normal/hitT deltas, groupshared FP16 preloads.
- Upscale pass at render res: 4-tap poisson bilateral (depth/normal
  weights), evaluates the SH with the *full-res* pixel normal (this is what
  restores normal-map detail), then a temporal filter with a per-pixel valid
  frame counter (`new = (n*prev + cur)/(n+1)`, n clamped). Output: full-res
  RGBA16F (or RGB9E5) irradiance texture.
- Composite: bound like the AO texture into set 2; the forward pass uses it
  as the indirect-diffuse term (multiplied by diffuse albedo) *instead of*
  the DDGI sample + SSGI when `rcgi` is on. `DebugView::kIndirectGi` shows it.

## Milestones

- **M1 (world side)**: light grid + radiance cache + irradiance cascades,
  plus a temporary full-screen debug composite that samples the cascades
  directly per pixel (DDGI-equivalent quality) so the world side is
  GPU-verifiable before the gather exists.
- **M2 (screen side)**: final gather + denoise + upscale + composite +
  debug UI/presets; replaces the M1 debug composite. GPU-verified vs DDGI.
  **Landed** (see "M2 implementation notes" below).

## Verification

Real GPU only (`vkrun`, host NVIDIA): `RX_RCGI=1 vkrun build/linux/runtime/rx
--demo cornell` (+ sponza) with `RX_UI_SHOT=<png> RX_UI_SHOT_FRAMES=120`;
compare against DDGI baseline shots; `DebugView::kIndirectGi` for the
isolated channel. lavapipe/swrun is unreliable (false-positive segfaults).

---

## M1 implementation notes (world side — landed)

M1 is implemented and GPU-verified (NVIDIA GB10, `vkrun`, zero validation
errors on/off): cascaded world light grid + spatial-hash world radiance cache +
cascaded octahedral irradiance/visibility volumes, plus the temporary full-res
resolve composite. `RX_RCGI=1` (or `RenderSettings::rcgi`) enables it; off by
default with zero behaviour change. When on it replaces the DDGI probe sample
**and** the SSGI pass in the forward indirect-diffuse term
(`kFrameFlagRcgi`, mesh.ps/mesh_rt.ps, inside the IBL branch — so, like DDGI,
RCGI only contributes when `settings.ibl` is on).

### Source map

- `engine/render/shaders/util/oct.hlsli` — shared octahedral encode/decode.
- `engine/render/shaders/gi/sh.hlsli` — 2-band SH (project + cosine irradiance),
  authored for M2's gather; not yet consumed in M1.
- `engine/render/gi/light_grid.{h,cc}` + `shaders/gi/light_grid.cs.hlsl` +
  `shaders/gi/light_grid.hlsli` — cascaded world light grid.
- `engine/render/gi/rcgi.{h,cc}` (`class RcgiSystem`) + shaders
  `gi/rcgi_common.hlsli`, `rcgi_probe_trace.cs.hlsl`, `rcgi_args.cs.hlsl`,
  `rcgi_cache_shade.cs.hlsl`, `rcgi_blend.cs.hlsl`, `rcgi_border.cs.hlsl`,
  `rcgi_resolve.cs.hlsl`.
- Wiring: `renderer.{h,cc}` (members `rcgi_`, `light_grid_`; `rcgi_active`
  predicate; passes after `tlas_build`; resolve after ssao; env-set + flag),
  `settings.h` (`rcgi`, `rcgi_intensity`), `host.cc` (carry `RX_RCGI` through the
  preset), `debug_ui.cc` (checkbox + intensity), `environment.{h,cc}` (env slot
  35), `mesh_pipeline.h` (`kFrameFlagRcgi = 1u<<16`), `mesh.ps`/`mesh_rt.ps`.

### (a) Buffers / textures M2 needs (owned by `RcgiSystem`, all in `kGeneral`)

Grid geometry: **16³ probes × 4 cascades**, spacing `2 m · 2^cascade` (cascade 0
= 2 m), camera-snapped per cascade with hysteresis; **one cascade updated per
frame**, round-robin `frame % 4`.

- `irradiance_` — `RGBA16Float`, **2560 × 640**. Cascaded octahedral irradiance
  atlas, **8×8 interior texels + 1-texel border (10×10)**, perceptual (sqrt)
  encoded (decode = square, like DDGI). Layout: `(texels+2)·probesX·probesZ`
  wide; per-cascade slab `(texels+2)·probesY = 160` tall; **4 slabs stacked
  vertically**, cascade `c` at rows `[c·160, (c+1)·160)`.
- `visibility_` — `RGBA16Float`, **2560 × 640**, same layout. `.rg` = hit-distance
  mean / mean² (Chebyshev at sample time). (Spec said RG16F; used RGBA16F to
  reuse the DDGI blend/border path and dodge storage-format support gaps —
  deviation noted below.)
- `rays_` — `RGBA16Float`, **32 × 4096** (rays × probes). Per ray: `rgb` = sky
  radiance on miss, `a` = signed hit distance: `>0` front hit, `<0` miss
  (`|a|` = distance), `0` backface.
- `state_` — `StructuredBuffer<uint>`, **1<<20 slots × 12 u32** (`kRcgiEntry`).
  Per-slot u32 offsets (see `rcgi_common.hlsli`): `0` key/checksum (0=empty),
  `1` instance(24)|geometry(8), `2` primitive, `3` bary.xy (2×f16), `4..6` world
  pos (f32), `7` world normal oct (2×f16), `8` hitT (f32), `9` last-shade frame,
  `10` last-queued frame, `11` pad.
- `radiance_` — `StructuredBuffer<uint2>`, **1<<20 slots**, packed HDR RGBA16F.
- `active_list_` (`uint`, 1<<18), `active_meta_` (`uint`, [0]=count),
  `dispatch_args_` (indirect) — per-frame active-cell queue for the shade pass.
- `globals_buffers_[2]` — host-visible UBO `RcgiGlobals` (see the header block in
  `rcgi_common.hlsli`), ping-pong by frame parity.

Light grid (`LightGrid`): `counts_` (`uint`, 16³·4), `ids_`
(`uint`, 16³·4·32, cell-major, 32 ids/cell), `params_buffers_[2]` (UBO:
per-cascade origin+cell-size). Cascade 0 spans 32 m (2 m cells), doubling.

Composite hand-off: the resolve pass writes the transient **`rcgi_irradiance`**
(`RGBA16Float`, render res), bound into **environment set 2, slot 35**
(`t35/s35`, black when off). M2 replaces `rcgi_resolve.cs.hlsl` with the
gather/denoise/upscale chain writing the same transient name/format/slot.

### (b) `rcgi_common.hlsli` — the M2-facing interface (resources passed as params)

- `SampleRcgiIrradiance(RcgiGlobals g, Texture2D irr, SamplerState irrS,`
  `Texture2D vis, SamplerState visS, float3 world_pos, float3 n, float3 v)`
  → trilinear 8-probe, Chebyshev-weighted irradiance in the smallest cascade
  containing `world_pos` (adapts `SampleDdgi`). Returns Lambert irradiance.
- `RcgiCacheLookup(RcgiGlobals g, StructuredBuffer<uint> state,`
  `StructuredBuffer<uint2> radiance, float3 world_pos, out float3 rad)` → bool
  valid; cached HDR radiance for the cell at `world_pos` (LOD by camera dist).
- Supporting (all pure or globals-as-param): `RcgiOct{Encode,Decode}`,
  `RcgiPack/UnpackOct`, `RcgiPack/UnpackRadiance`, `RcgiFibonacci`,
  `RcgiProbeFromIndex`/`RcgiProbeIndex`/`RcgiProbePosition`, `RcgiAtlasSize`,
  `RcgiSlabHeight`, `RcgiAtlasUv`, `RcgiSelectCascade`, `RcgiHashScalar`,
  `RcgiCellHash`, `RcgiCellChecksum`, `RcgiCacheCell`.
- Cache cell: `lod_exp = floor(log2(1 + dist/lodDist))` (`lodDist = 8 m`),
  `cellSize = 0.25 · 2^lod_exp`, key = chained integer hash of (quantized int3,
  lod_exp); linear-probe 8 slots, checksum 0 = empty.
- `light_grid.hlsli`: `LightGridCell(LightGridParams grid, float3 pos, out uint
  flat_cell)` → smallest cascade cell; iterate `lg_counts[cell]` ids in
  `lg_ids[cell·32 + i]` into the bound `Light` buffer.

### (c) Frame pipeline (all synchronous in M1) & known limitations / TODOs

Pipeline: `light_grid` → `rcgi` pass (probe trace → `rcgi_args` → cache shade
[indirect] → blend irr+vis → border) → prepass → `rcgi_resolve` → scene.

- **Not async in M1** (spec-permitted): the world passes run on the main
  timeline. M2 (or a follow-up) can `b.Async()` them with a `JoinAsync` before
  the resolve, like DDGI.
- **Visibility atlas is RGBA16F**, not RG16F (reuses DDGI blend/border, avoids
  RG16F storage-image support gaps). Only `.rg` is meaningful.
- **RCGI is IBL-gated** (composites in the `kFrameIbl` branch, mirroring DDGI);
  IBL-off scenes (e.g. the point-light `lights` demo) get neither DDGI nor RCGI.
  Verified the light-grid → cache → cascade point-light path by temporarily
  forcing IBL on in that demo (reverted): colored bounce pools appear at light
  bases and `rcgi` runs.
- **Cache hit0 packs geometry index in 8 bits** (≤255 geometries/mesh) and
  instance in 24 bits (≤16M) — fine for current content.
- **Blend fallback** on a cache miss (hash overflow / eviction) — **CLOSED**
  (Phase 5 item 20b): the blend pass now falls back to `RcgiSkyMiss` (sky
  outdoors, interior ambient indoors) instead of black, so an un-shaded hit no
  longer darkens the cascade. `rcgi_blend.cs.hlsl` gained the sky cube (slot 6).
- **Transparent surfaces** get no RCGI in M1 (only the opaque scene env-set is
  wired); DDGI wired transparents, so this is a small gap under RCGI. Easy
  follow-up (add the trailing arg to the transparent `WriteEnvSet`). *(Closed in
  the M2 "transparent gap closed" note below.)*
- Reflections/spec-refl bounce reading the DDGI atlas (black under RCGI) —
  **CLOSED** (Phase 5 item 20b); see the Phase 5 section.

### (d) Measured GPU cost (1920×1011, `RX_GPU_TIMINGS=1`, GB10)

- `rcgi` (world side: trace + shade + blend + border, one cascade/frame): ~0.20–0.23 ms.
- `rcgi_resolve` (full-res M1 filler): ~0.29–0.52 ms (scene dependent).
- `light_grid`: negligible (folded into "other"; test scenes bin few/no lights).

Verified: `--demo cornell` (red/green wall bleed present, matches DDGI baseline
in `DebugView::kIndirectGi`), `--demo materials` (clean, no speckle/seams),
`--demo lights` (light-grid path, IBL-forced), `--no-rt` (silently unavailable,
no crash), and `--validation` on+off (zero errors from RCGI passes).

---

## M2 implementation notes (screen side — landed)

M2 replaces the M1 full-res cascade resolve with the idTech8-style screen-side
chain: **half-res SH final gather → separable bilateral denoise → full-res
poisson upscale + temporal filter**, writing the same `rcgi_irradiance`
transient (env set 2 slot 35). GPU-verified on NVIDIA GB10 (`vkrun`), `--demo
cornell`/`materials`/`lights`, async on+off, `--validation` clean (from M2
passes). `RX_RCGI=1` enables; off by default with zero behaviour change (no
gather, no history copies — all gated on `rcgi_world`).

### Passes (all compute; `RcgiSystem::AddGatherChain` / `AddHistoryCopy`)

- **`rcgi_gather`** (`rcgi_gather.cs.hlsl`, half res = render/`kGatherDivisor`,
  divisor a code constant, `4` = quarter-res ready). Reconstructs world pos from
  the reversed-Z depth + inv_view_proj, oct-decodes the prepass normal, traces
  **1 cosine-weighted ray** (inline `RayQuery`, `RAY_FLAG_FORCE_OPAQUE`, mask
  `RX_RAY_MASK_REALTIME`, TLAS like the M1 probe trace) with per-pixel/per-frame
  hash noise + a normal-offset origin bias. Sky pixels bail (zero SH). Radiance
  from the three-level fallback: **(a) screen cache** — project the hit with
  `prev_view_proj`, accept if the reversed-Z depth matches the stored previous
  depth within a relative epsilon (linearised `near/d`), take the previous lit
  HDR colour; **(b) `RcgiCacheLookup`** at the hit; **(c) `SampleRcgiIrradiance`**
  with the negated ray dir as the geometric-normal approximation (no geometry
  fetch). Ray miss = sky cube. Radiance is firefly-clamped and projected into
  2-band SH along the ray dir (`sh.hlsli`, `weight = pi/max(cos,0.1)` — the
  cosine-pdf 1/p estimate). Outputs 3× RGBA16F SH transients + R16F hitT.
- **`rcgi_denoise`** (`rcgi_denoise.cs.hlsl`, gather res, H then V, ping-pong
  A↔B). Separable gaussian (radius 5) with bilateral weights from linear-depth,
  normal dot, and a **relative, gentle** hitT term (the per-pixel single-ray
  hitT is noisy — a hard hitT weight rejects every neighbour; contact-scale
  detail, center hitT < 1 m, sharpens it).
- **`rcgi_upscale`** (`rcgi_upscale.cs.hlsl`, full res). 4-tap bilateral poisson
  upsample of the denoised SH (depth/normal agreement, nearest-valid fallback),
  evaluated with the **full-res** normal (`ShIrradiance`, restores normal
  detail, negative lobes clamped) → RGB irradiance. Temporal filter: motion
  reproject, per-pixel counter in history alpha (`new=(n·hist+cur)/(n+1)`, n≤48),
  reset on out-of-bounds; **generous** neighbourhood clamp (the 4 upsample taps
  come from one denoised frame — a tight box pins history to the noisy current
  sample and blocks convergence; GI is low-freq and TAA follows). Writes
  `rcgi_irradiance = accum · rcgi_intensity` (intensity applied here, once, as
  the M1 resolve did) plus the raw-irradiance temporal history.
- **`rcgi_history`** (`rcgi_history.cs.hlsl`, full res, end of lit frame). Copies
  the composited lit HDR colour + depth into the persistent screen-cache images
  for next frame's gather. Recorded only when the M2 gather ran.

### Persistent resources (owned by `RcgiSystem`, render res, `ImportImage`)

- `screen_color_hist_` (RGBA16F) + `screen_depth_hist_` (R32F): the screen-cache
  history, written late (`AddHistoryCopy`) and read early next frame (gather).
  Single images — read-before-write in the same frame is the last-frame content.
- `irr_hist_[2]` (RGBA16F): temporal irradiance history, ping-pong by frame
  parity (`new=(n·hist+cur)/(n+1)`, sample count in `.a`).
- Created/resized by `EnsureScreenResources`, primed to `kGeneral`. `first_frame`
  / resize / teleport (`RequestReset`) drops screen-cache + temporal history.

### Debug toggle

- **`RX_RCGI_PROBES_ONLY=1`** (`rcgi.probes_only`) swaps the gather chain for the
  retained M1 per-pixel cascade resolve — near-free A/B (the resolve pipeline is
  still compiled). `DebugView::kIndirectGi` (`RX_DEBUG_VIEW=6`) shows the M2
  chain has clear per-pixel contact occlusion / directional bleed near geometry
  where the probes-only path is flat and blurry.

### Async compute

- **Enabled** and default-on (`settings_.async_compute`, `RX_ASYNC_COMPUTE`,
  gated on `device.caps().async_compute`). The M1 world passes (light grid +
  probe trace → args → cache shade → blend → border) fork onto the async queue
  via `b.Async()`; an empty `async_join` `b.JoinAsync()` pass sits before the
  gather (their first consumer), mirroring DDGI. The `kGeneral` world resources
  ride the M1 manual-barrier discipline across the join fine — validated clean
  with async **on and off**. RCGI and DDGI are mutually exclusive
  (`ddgi_active = … && !rcgi_active`), so only one async fork is live.

### Transparent gap closed

- The transparent surfaces' `WriteEnvSet` now receives the `rcgi_irradiance`
  view (was black under RCGI), so glass/particles get RCGI ambient. Verified in
  `--demo materials` (front glass spheres lit, not black).

### Measured GPU cost (1920×1011, GB10, steady state)

- `rcgi_gather` + `rcgi_denoise` (H+V) + `rcgi_upscale` + `rcgi_history` ≈
  **1.2–1.4 ms** combined (each pass ~0.3–0.4 ms steady state). Under the
  ~1.5 ms target. NB: the `RX_UI_SHOT` capture frame stalls whichever pass
  overlaps the readback fence, so a single overlay sample can spike one pass to
  ~1.5–1.9 ms — that is a capture artifact, not steady-state cost.
- Temporal convergence (static cornell camera): back wall/floor essentially
  frozen (0 % pixels change > 6/255 across a 6-frame gap); the bright mid-floor
  keeps a small residual (~2–3/255 mean) that TAA further smooths.

### Known limitations / TODOs

- **Reflection/spec-refl bounce** reading the DDGI atlas (black under RCGI) —
  **CLOSED** (Phase 5 item 20b): the reflection hit's indirect-diffuse term now
  samples the RCGI irradiance cascades when RCGI is active. See Phase 5.
- **Quarter-res gather** — **CLOSED** (Phase 5 item 23): exposed as
  `RX_RCGI_GATHER_SCALE` (2 = half default, 4 = quarter), denoise radius widens
  automatically at quarter. See Phase 5.
- **RGB9E5** irradiance storage still deferred (RGBA16F throughout).
- **Screen-cache validation** uses a linearised `near/d` reversed-Z depth
  compare (no prev-frame inverse matrix in the push budget); robust in practice
  but a full prev-world reconstruction would be tighter.
- **Pre-existing (not M2)**: the M1 `rcgi_args` indirect-dispatch barrier trips
  a validation warning (`VkMemoryBarrier2` INDIRECT_COMMAND_READ under a compute
  dst stage — an rhi-layer `BarrierScope::kIndirectArgs` translation issue that
  only surfaces on the **async compute queue** (the `compute_only_` stage filter
  drops `DRAW_INDIRECT` and falls back to `COMPUTE_SHADER`); engine-wide, left
  untouched — clean with async off). M2 fixed the M1 `vkCmdFillBuffer`
  TRANSFER_DST errors by adding `kBufferUsageTransferDst` to the filled buffers.

---

## Review fixes (PR #12)

Behavioral changes landed while addressing the PR review; the notes above are
updated inline where they described the old behavior.

- **Lazy creation.** `RcgiSystem` + `LightGrid` (~85 MiB) are now created on the
  first activation in `Renderer::ApplySettings` (`settings_.rcgi && rt_available_
  && bindless_ && !rcgi_`), not eagerly on every RT-capable device. Creation
  failure is non-fatal: it logs, leaves `rcgi_` null (feature silently
  unavailable), and latches `rcgi_create_failed_` so it is not retried each
  frame. Created-once: toggling RCGI off keeps the allocation (no 85 MiB thrash).
  A `RX_INFO("rcgi: created on first activation")` line fires only when it is
  actually enabled.
- **Frame-stamp encoding.** The per-entry shade stamp (slot 9) and queued stamp
  (slot 10) are stored as `frame_index + 1` (`RcgiStampEncode`), so a cleared
  slot (0) reads unambiguously as "never" and frame 0 is not aliased with the
  empty sentinel. `RcgiCacheLookup` now rejects a key-matched entry whose shade
  stamp is still 0 (published by the probe trace but not yet shaded → undefined
  radiance), falling through to the cascade/sky level.
- **Cache payload ownership.** Exactly one ray per cell per frame claims the
  entry via `InterlockedExchange` on the queued stamp and writes the full
  multiword payload; losers write nothing, so the payload can no longer be torn
  across rays. The shade pass also bounds-checks the unpacked instance/geometry/
  material indices against the record counts before any bindless fetch.
- **Age-based eviction.** During the insertion probe, a slot owned by a different
  cell whose queued stamp is older than `kRcgiEvictAge` (256 frames, in
  `rcgi_common.hlsli`) is reclaimed with a key `InterlockedCompareExchange`; the
  winner zeroes the shade stamp so the previous occupant's radiance cannot leak.
  ABA is tolerated (cache semantics). This replaces "insert only accepts
  empty-or-matching slots; stale cells accumulate until probing rejects samples".
- **Per-cascade validity.** Only the round-robin `frame % 4` cascade blends each
  frame, so first-blend reset and sample validity are tracked per cascade
  (`cascade_valid_[4]`), not a single global `history_valid_`. A cascade's first
  blend after creation/teleport runs with `reset=true` (hysteresis 0). A
  per-cascade valid bitmask is uploaded in `RcgiGlobals::valid.x`; the shared
  `SampleRcgiIrradiance` returns zero from a not-yet-valid cascade. The atlases
  are additionally cleared to black at creation (they now carry
  `kTextureUsageTransferDst`) so even a pre-blend read is 0, not garbage.
- **Barrier scopes.** The post-`FillBuffer` clear barriers for `state_` /
  `active_meta_` now target `BarrierScope::kComputeReadWrite` (new RHI scope =
  compute read + storage write), because the probe trace consumes those buffers
  through `InterlockedCompareExchange`/`InterlockedAdd`, which read as well as
  write. (`light_grid` writes `cell_counts` per-thread with no clear, so no
  equivalent hazard.)
- **Screen-resource failure & upscale robustness.** `EnsureScreenResources`
  returns `bool`; on failure it tears down partial allocations, clears the cached
  extent (so a later frame retries), and the caller falls back to the
  probes-only resolve — no null images are imported. The upscale pass treats an
  all-sky tap neighborhood (inverted `mn > mx`) as a disocclusion (zero
  irradiance, history reset, temporal clamp skipped) and guards its output to a
  finite non-negative range so history can never go non-finite.
- **Shutdown order.** `Renderer::Shutdown` resets `rcgi_` (and destroys
  `light_grid_`) before `device_` teardown, next to `ddgi_`, so `~RcgiSystem`
  no longer runs against a destroyed `Device&`.

---

## Phase 3 — leak & occlusion hardening (AC Shadows adoption, items 9-11)

Three additions that stop RCGI leaking light where the sparse cascades cannot
see occlusion. All default on, each behind its own env for A/B, and all no-ops
for existing scenes (zero cost when their inputs are absent).

### 9. Indoor/outdoor classification (`RX_RCGI_INTERIOR`, default on)

- **(a) Global interior mode.** When the app sets `RenderSettings::interior`,
  RCGI's ray misses (`RcgiSkyMiss` in `rcgi_common.hlsli`) now return the
  authored `interior_ambient` instead of the sky cubemap, in the probe trace and
  the gather alike. The probe cascades stop being fed skylight through
  ceiling/doorway gaps. The interior flag + ambient ride the globals UBO
  (`RcgiGlobals::interior`/`gi_flags`), set from `FrameConfig` in `AddToGraph`.
  The forward pass gained an interior+RCGI branch (`mesh.ps`/`mesh_rt.ps`): with
  RCGI on indoors it lights the interior with leak-free bounce rather than the
  flat authored term (gated so RCGI-off scenes are unchanged).
- **(b) Interior volume list.** `Renderer::SetInteriorVolumes(span<InteriorVolume>)`
  uploads <=64 world AABBs to a small host-visible buffer. `SampleRcgiIrradiance`
  classifies the sample position and each of the 8 blend probes indoor/outdoor
  (`RcgiPointInInterior`) and near-zeroes the weight of cross-class probes,
  killing the outdoor-probe-through-a-doorway leak for mixed indoor/outdoor
  scenes (and future Skyrim room bounds). Costs nothing when no volumes are set.

### 10. Probe relocation + backface disable (`RX_RCGI_RELOCATE`, default on)

- Per (cascade, probe) metadata buffer `probe_meta_` (uint2: packed +-0.45-cell
  offset + a disabled flag). The new `rcgi_probe_meta.cs.hlsl` pass runs after the
  border pass, reads back this frame's rays, and nudges each probe toward the open
  hemisphere (away from backface directions, weighted by ray openness) when the
  backface fraction exceeds ~25%, disabling it past ~60%. The offset is applied
  identically in the probe trace (ray origins), the blend (cache-key
  reconstruction) and `SampleRcgiIrradiance` (interpolation), so world positions
  agree; the offset is reset when a cascade snaps (its cells reassign). Offset
  packing has a CPU mirror in `render/gi/rcgi_interior.h` (unit-tested).

### 11. Probe AO (`RX_RCGI_PROBE_AO`, default on)

- In the gather, an irradiance-cascade fallback hit is attenuated by
  `saturate(hitT / cascadeSpacing * scale + bias)` (defaults 0.6 / 0.4),
  recovering contact occlusion the sparse probes cannot represent. Applies only
  to the cascade fallback -- screen-cache and hash-cache hits carry their own
  occlusion.

### Not landed

- **12 (RTAO from the gather hitT)** and **13 (secondary sun shadow map for the
  SW tier)** are deferred; see the commit/PR notes for the plumbing plan (item 12
  needs the gather hoisted above the NRD AO block, or a persisted previous-frame
  hitT, to resolve the ordering).

### Verification

`--demo interior` (an enclosed room + door gap + lamp, flagged interior and
forwarded as a volume) exercises all three on NVIDIA GB10 (`vkrun`): the room
lights from the lamp with leak-free interior ambient (measurably so vs
`RX_RCGI_INTERIOR=0`), `--demo cornell` GI color-bleed is unchanged, software
(`RX_RCGI_SW`) runs, and validation is clean apart from the pre-existing async
indirect-args VUID. CPU unit test: `rcgi_interior_test`.

---

## Phase 4 — specular quality & perf (AC Shadows adoption, items 14-19)

Reflection-pass levers on top of the existing full-res VNDF specular trace
(`engine/render/screenspace/reflection_trace.{h,cc}` +
`shaders/screenspace/reflection_trace.cs.hlsl`, denoised by NRD REBLUR_SPECULAR).
Each has its own env for A/B, all default on. The reflection pass is NRD-gated
(`spec_refl_active` needs `nrd_.available()`), so everything here is inert
without NRD. Landed: **14, 16, 17, 19**. Deferred with specs below: **15, 18**.

### 14. Half-res reflections + bilateral upscale (`RX_REFL_HALF`, default on)

- The trace runs at **half × half** resolution (quarter the rays -- the dominant
  reflection cost is the TLAS ray, not the denoise), then a depth/normal-weighted
  **bilateral upscale** (`reflection_upscale.cs.hlsl`, the RCGI-upscale weighting
  pattern) reconstructs full res before NRD consumes it.
- **NRD stays full-res.** REBLUR's resolution is fixed at instance creation
  (render res) and the vendored integration does not expose REBLUR's
  checkerboard/half-res mode, so the honest choice is: trace half, upscale to full
  (edge-aware, so reflections do not bleed across silhouettes), then denoise full
  as before. NRD guides (normal_roughness, viewZ, motion) stay full-res and
  consistent. The perf win is the 4× ray reduction; the upscale is one cheap pass.
- **Half×half over half-width**: NRD's temporal/spatial reuse handles the extra
  noise from a 1-spp quarter-density input well, and half×half is the larger ray
  saving. The guides are full-res; the shader maps each half-res output pixel to a
  full-res guide texel by a per-frame-jittered `step` so the half-res selection
  does not lock onto one quadrant.
- Verified `--demo materials` on NVIDIA GB10: half-res is visually
  indistinguishable from full-res (`RX_REFL_HALF=0`) -- chrome spheres reflect
  sharply, glossy blur is preserved, no edge smear.

### 16. Specular ray-skip via diffuse SH (`RX_REFL_SH_SKIP`, default on)

- Before tracing, if `roughness > sh_skip_rough` (`RX_REFL_SH_SKIP_ROUGH`, 0.45)
  **or** `dot(rayDir, mirrorDir) < 0.5`, the trace is replaced by evaluating the
  **RCGI gather's per-pixel denoised diffuse SH** along the ray direction
  (`ShEvaluate` in `sh.hlsli` -- the raw radiance reconstruction, not the
  cosine-convolved `ShIrradiance`, clamped non-negative). The hard
  `roughness_cutoff` (0.6) stays as the outer bound; this is the softer,
  directionally-aware cull inside it.
- **Requires the RCGI gather chain.** `RcgiSystem::AddGatherChain` now stashes its
  final denoised SH triple (`a_r/a_g/a_b`, gather res) and exposes them via
  `RcgiSystem::denoised_sh()`; the probes-only/software resolve produces none
  (returns false). The renderer was **reordered** so the RCGI screen side runs
  *before* the NRD/reflection block (RCGI and DDGI are mutually exclusive, so this
  never races the DDGI async join the reflection path uses). When RCGI is off the
  SH slots bind a harmless placeholder and the flag stays clear -> current
  behaviour (full trace, DDGI indirect).
- The reflection pass reads the SH via full-res uv -> gather texel, robust to any
  output/gather ratio (with item 14 both are render/2, a near 1:1 map).
- Verified `--demo materials` + `RX_RCGI=1`: SH-skip on vs `RX_REFL_SH_SKIP=0` is
  indistinguishable -- no darkening of mirror/glossy surfaces.

### 17. Roughness-scaled ray length

- `ray.TMax = maxDist * ((1-roughness)^2 + 0.1)` (their formula; `maxDist` = 200,
  `Frame::max_ray_dist`). Rough surfaces reflect only the near neighbourhood, so
  the ray shortens and the miss (sky/SH) fills the far field the blur swallows
  anyway. The same reach seeds the SH-skip's normalised hit distance.

### 19. One-step fog on specular hits (`RX_REFL_FOG`, default on)

- On a world-trace **hit**, an analytic exponential-height-fog transmittance is
  integrated **once** over the reflected segment `[origin, hitPos]`
  (`HeightFogTransmittance`; closed form `d0·(1-e^{-b·dir.y·L})/(b·dir.y)`), and
  the hit radiance is faded toward the horizon inscatter colour (the prefiltered
  sky cube along the ray at a blurry mip, the same colour a miss samples). This
  removes the hard cut where distant reflections would meet the raster fog line.
- **Consistent with the raster fog**: gated on `fog_active` (RX_FOG) and reuses
  `settings_.fog_density / fog_height_falloff / fog_base_height`. The segment
  starts at the reflected surface, so eye->surface fog (already applied on the
  primary) is not double-counted -- near-camera fog is skipped by construction.
- Verified `--demo water RX_FOG=1`: fog renders, reflections fade to horizon, no
  artifacts (subtle -- the effect only touches reflected-ray hit segments).

### Files

- `shaders/screenspace/reflection_trace.cs.hlsl` (half-res mapping, ray-skip,
  roughness reach, hit fog; push grew to 208 B), new
  `shaders/screenspace/reflection_upscale.cs.hlsl`, `shaders/gi/sh.hlsli`
  (`ShEvaluate`), `screenspace/reflection_trace.{h,cc}` (upscale pipeline, SH +
  half + fog Frame fields, static_asserts on push size), `gi/rcgi.{h,cc}`
  (`denoised_sh` accessor + stash), `core/renderer.cc` (RCGI-before-reflections
  reorder, `RX_REFL_*` envs, wiring), `engine/render/CMakeLists.txt`.

### Verification

Built with NRD (`tools/get_nrd.sh`), `cmake --preset linux`, zero new warnings,
`ctest` 19/19. GPU smoke on NVIDIA GB10 (`vkrun`, `--demo materials` /
`--demo water`): reflections correct with every `RX_REFL_*` env on/off and with
RCGI on/off; validation clean apart from the pre-existing async indirect-args
VUID. Per-pass frame-time delta (half vs full) is within the capture-frame
readback-stall noise in these small demos -- not reliably measurable here; the
saving is the 4× ray-count reduction.

### Deferred — 15 (screen-space-first hybrid) and 18 (far scene cubemap)

Both are the L-effort items; deferred to keep 14/16 landing verified. Specs:

**15. Screen-space-first hybrid tracing (`RX_REFL_SS_FIRST`).** Before the TLAS
ray in `reflection_trace`, ray-march the depth buffer along `dir` (adapt the SSR
marcher `ssr.cs.hlsl`: 16-24 linear steps + binary refinement). On a valid hit
(ray depth vs z-buffer within a world-space thickness) shade from the
**previous-frame lit HDR** at that pixel (matches RCGI's screen cache; cheaper
than a gbuffer relight) and skip the TLAS ray. If the march leaves the screen or
passes behind geometry beyond threshold, **backtrack** to the last valid position
and start the TLAS ray from there (AC Shadows' key trick -- avoids the
raster-depth vs coarse-RT self-intersection disparity). Their numbers: ~38% of
rays resolve on screen. Plumbing needed: bind a prev-frame lit HDR colour + depth
to the reflection pass. RCGI already owns `screen_color_hist_`/`screen_depth_hist_`
(render res, written by `AddHistoryCopy`), but they are RCGI-owned and only valid
under RCGI; a general path wants a renderer-owned prev-lit-colour history (or gate
SS-first on RCGI, as SH-skip does). *Risk*: the backtrack + thickness tuning and
the RCGI-ownership of the history are why it is deferred rather than the marcher
itself. The same SS-first march would help the RCGI **gather** ray more (bigger
win), but the gather already has a screen radiance cache for HIT reprojection, so
add it there only after the reflections version is tuned.

**18. Far scene cubemap (`RX_FAR_CUBE`).** A 128² RGBA16F cubemap centred on the
camera, **one face/frame round-robin**, rasterising the real scene coarsely (sky +
geometry beyond the near range, lowest LODs, tiny instances culled by the Phase 2
solid-angle culler at a much more aggressive threshold; recreation's `.btr/.bto`
distant-LOD proxies are the geometry to render into it). Cheapest shading that
reads plausibly: depth + albedo·(ambient + sun N·L), no shadows. It replaces the
atmosphere-only prefiltered cube as the reflection **miss** fallback, sampled with
the distance-warped direction `normalize((rayOrigin + rayDir*fakeHitDist) -
cubemapPos)` so distant mountains/buildings appear in water/mirror reflections
instead of vanishing; also the RCGI probe-trace/gather miss (but the Phase 3
interior mode still wins indoors -- `RcgiSkyMiss` must branch interior first).
Plumbing needed: a persistent cube target + 6 face-view matrices, a minimal
per-face raster pass (its own small pipeline reusing the mesh vertex path), a
round-robin face index in the frame state, and the reflection/RCGI miss sample
sites swapped from `prefiltered_cube` to the far cube. *Risk*: the new raster
target + per-face pass + frame-graph placement is the M-effort plumbing that
pushes it past this phase's verified budget; the sampling-site swap itself is
small.

---

## Phase 5 — ship it in recreation (AC Shadows adoption, items 20-23)

RCGI becomes a shippable default GI: it is wired into the high/ultra quality
presets and recreation's ray-traced trailer mode, the two "black under RCGI"
TODOs are closed, denoiser material-ID masks land, and the gather gains a
quarter-res knob. **Landed & GPU-verified: 20, 22, 23.** **Deferred with spec
below: 21 (thin translucency).**

### 20. RCGI as the quality-preset GI

- **(a) Presets** (`engine/render/core/presets.cc`). `kUltra` and `kHigh` now set
  `s.rcgi = true`; `s.ddgi = true` stays as the automatic fallback (the renderer's
  `rcgi_active` predicate picks whichever is available, and the two are mutually
  exclusive). `kMedium`/`kConsole` remain ddgi, low tiers ssgi — mirroring the
  AC Shadows platform ladder. `s.rcgi = false` is forced in the no-ray-query
  clamp. **RX_RCGI wins in both directions** but only when explicitly set:
  `host.cc` now guards the env carry with `Renderer::rcgi_env_overridden()`
  (`RcgiOpt.overridden()`), so an unset env no longer clobbers a preset that
  enabled rcgi. Set on ultra/high because the measured gather chain is ~1.2-1.4 ms
  on GB10 — within those tiers' budget.
- **(b) Spec-bounce reads RCGI** (was "black under RCGI"). The reflection hit's
  indirect-diffuse term (`reflection_trace.cs.hlsl`, `SampleHitIndirect`) samples
  the RCGI irradiance cascades (`SampleRcgiIrradiance`, leak-hardened
  classification) when `kFlagRcgi` is set, else the nearest DDGI probe as before.
  `RcgiSystem::irradiance_binding(frame_index)` bundles the cascade atlases +
  globals + probe-meta + interior-volume buffers; the renderer binds the real
  resources when RCGI is active (`in_general`, atlases live in kGeneral) and
  environment placeholders (`black_view`/`dummy_volume`/`dummy_storage`) otherwise
  so the descriptor set is always complete. Slots 10-14 added to the reflection
  pipeline. Verified `--demo materials RX_RCGI=1`: reflective spheres show lit,
  coloured inter-sphere bounce (was black indirect).
- **(b) Cache-miss blend fallback** (was "black not sky/probe"). `rcgi_blend`
  gained the sky cube (slot 6); a probe-ray hit whose radiance is not cached yet
  falls back to `RcgiSkyMiss` (sky outdoors, interior ambient indoors) instead of
  black. The gather already fell through to the cascades on a cache miss, so this
  closes the remaining darkening path (the multi-bounce feed). Reading the
  written irradiance atlas back was rejected as a WAR hazard; the sky/interior
  fallback is the hazard-free choice the TODO endorsed.
- **(c) recreation** (`runtime/camera_input.cc`, `runtime/debug_ui.cc`). The
  trailer `kRayTracing` mode sets `s.rcgi = true` alongside `s.ddgi = true`
  (fallback); `kPathTracing` sets it too (inert while `path_trace` gates
  `rcgi_world` off); `kRaster` clears both `rcgi`/`ddgi`. A "RCGI
  (radiance-cached GI)" checkbox + intensity slider sit above the DDGI checkbox
  in the debug UI's IBL section (both ray-query/IBL gated).

### 22. Denoiser masks & vegetation disocclusion

- **(a) Material-ID mask.** The prepass (`prepass.ps.hlsl`, shared by the masked
  variant) writes a per-pixel class into the otherwise-unused normal `.a`
  channel: opaque (0), vegetation (1, `kFlagWind`), character (2,
  `kFlagSkin`/`kFlagHair`). Translucent (3) is reserved (blend materials are not
  in the opaque prepass). Shared decode in `shaders/gi/material_class.hlsli`.
  `rcgi_denoise` and `reflection_upscale` reject cross-class neighbours as a hard
  bilateral weight (`RxMatClassMatch`), so wind/character indirect no longer
  smears onto opaque surfaces during spatial filtering. Gated by
  `RX_RCGI_DENOISE_MASK` (default on) for A/B.
- **(b) Vegetation disocclusion** (`rcgi_upscale` temporal filter). Vegetation
  pixels soften the spatial normal weight (wind normals vary), search a 16-tap
  history neighbourhood (picking the most-converged sample) before declaring a
  disocclusion, and loosen the temporal clamp; on a genuine disocclusion one
  extra 8-tap poisson spatial pass (`VegDisoccFill`, same-class only) knocks down
  the single-frame noise.
- **(c) NRD material ID.** `nrd_pack.cs.hlsl` forwards the class into
  `NRD_FrontEnd_PackNormalAndRoughness`'s material-ID slot (the vendored REBLUR
  builds with `NRD_NORMAL_ENCODING=2` = R10_G10_B10_A2_UNORM, which carries the
  2-bit id 1:1). It is **harmless while the material test is disabled** (REBLUR's
  `minMaterialForSpecular`/`Diffuse` default to 4.0 ≥ the max class 3, so all
  comparisons pass) and ready to enable if cross-material spec bleed needs it —
  enabling is a one-line settings change left off pending an A/B, per the item's
  "skip and note if not trivial".
- The demos carry no wind/skinned materials, so the mask is a no-op there (all
  class 0); it renders clean on/off. The visible win needs a vegetation/character
  scene (recreation forest / NPCs) — flagged as the standing test for a later
  screenshot pass.

### 23. Quarter-res gather

- `RX_RCGI_GATHER_SCALE` (setting `rcgi.gather_scale`): 2 = half res (default),
  4 = quarter. `RcgiSystem::set_gather_scale` drives the runtime `gather_divisor_`
  (replacing the compile-time `kGatherDivisor`). At quarter the separable denoise
  radius widens 5 → 9 (its gaussian sigma tracks the radius) to compensate for the
  ~4× fewer rays per full-res pixel; the 4-tap bilinear upscale is a correct
  reconstruction and needs no change. A/B on GB10 (`--demo cornell`/`interior`):
  quarter preserves the colour bleed with **no visible splotching**, at a lower
  `rcgi` gather cost (the `rcgi_upscale`/full-res passes are unchanged). Default
  stays half; quarter is the opt-in console knob.

### Envs added

- `RX_RCGI_GATHER_SCALE` (2/4) — gather resolution (item 23).
- `RX_RCGI_DENOISE_MASK` (0/1, default 1) — material-ID denoiser mask (item 22).

### Files

- rx: `core/presets.cc`, `app/host.cc`, `core/renderer.{h,cc}`,
  `gi/rcgi.{h,cc}`, `atmosphere/environment.h`,
  `screenspace/reflection_trace.{h,cc}`, shaders
  `screenspace/reflection_trace.cs.hlsl`, `screenspace/reflection_upscale.cs.hlsl`,
  `gi/rcgi_blend.cs.hlsl`, `gi/rcgi_denoise.cs.hlsl`, `gi/rcgi_upscale.cs.hlsl`,
  `gi/nrd_pack.cs.hlsl`, `pipeline/prepass.ps.hlsl`, new
  `gi/material_class.hlsli`.
- recreation: `runtime/camera_input.cc`, `runtime/debug_ui.cc`.

### Verification

Built inside `nix develop` (`cmake --preset linux`, NRD via `tools/get_nrd.sh`),
zero new warnings, `ctest` 19/19. GPU smoke on NVIDIA GB10 (`vkrun`):
`--demo cornell`/`interior`/`materials` with `RX_RCGI=1`, `RX_RCGI_GATHER_SCALE`
2/4 and `RX_RCGI_DENOISE_MASK` 0/1 — colour bleed correct, reflections show lit
RCGI bounce, interior leak-hardened with no cache-miss over-bright, quarter-res
clean. No new validation errors (the async indirect-args VUID is pre-existing).
recreation `camera_input.cc`/`debug_ui.cc` object-compile clean against the rx
worktree.

### 21. Thin translucency at RT hits — DEFERRED (spec)

Genuinely L-effort; deferred because it cannot be GPU-verified in this budget
without a thin-translucency test scene (the shipped demos have none, and a
Skyrim barred-window interior needs game data). The mask-matrix interaction and
the probabilistic front/back pdf rebalance are exactly the parts that need a
reference A/B, so landing it unverified would be worse than a precise spec.
Scope is **thin** translucency only (shoji/paper/leaves), not thick glass.

**Predicate.** A conservative "thin translucent" test: blend alpha mode **and**
double-sided **and** a transmission/translucency hint (`> 0`), or an explicit
`is_thin` material flag. Add `kFlagThin` (material_system) + a `translucency`
scalar `t` (0..1) to the simplified `MaterialRecord`.

**BLAS routing (mind the Phase 1/2 mask matrix).** Thin materials are currently
excluded from the BLAS entirely; blend geometry is not in any realtime ray path
(diffuse rays trace `RX_RAY_MASK_REALTIME|APPROX` with `CULL_NON_OPAQUE +
FORCE_OPAQUE`). Add a new mask bit `RX_RAY_MASK_THIN` and include thin submeshes
as **non-opaque** geometry on that bit only (never on REALTIME/APPROX, so the
existing force-opaque diffuse rays skip them). The simplest correct consumer is a
**translucency-aware gather variant**: the gather ray traces
`REALTIME|APPROX|THIN` **without** `FORCE_OPAQUE` and runs an any-hit/`RayQuery`
candidate loop so thin hits are transmissive rather than solid walls. Keep the
path tracer on the real (non-approx) geometry as today.

**Primary surface (gather).** Requires knowing the primary pixel is thin —
blend materials are not in the opaque prepass, so either (a) tag thin geometry
into the prepass gbuffer with material class 3 (needs the prepass to admit thin
draws — the larger sub-change), or (b) restrict the primary-surface split to the
secondary path and treat primary thin surfaces via the existing transparent
forward pass. With the class available: at a thin primary surface pick the
front/back hemisphere with probability `p = t / (1 + t)` (back) vs `1 - p`
(front), trace the chosen hemisphere, and **divide the result by the pdf**
(`1/p` or `1/(1-p)`); for a back-hemisphere ray, **flip the ray direction when
projecting into SH** so the encoded lobe matches the shaded (front) normal —
their exact scheme.

**Secondary hits (`cache_shade`).** At a thin-geometry hit, light **both faces**:
shade with the geometric normal and again with `-normal` (backface), and
attenuate the back contribution by `t · albedo` (the transmitted fraction). This
is the interior-window-light win — sun/skylight leaks through shoji/paper into
the room's cache radiance.

**Plumbing.** `raytracing.{h,cc}`/`material_system` (predicate, `kFlagThin`,
`t`, THIN BLAS variant on the new mask bit), `rt_geometry.hlsli` (unchanged),
`rcgi_gather` (THIN ray + pdf rebalance + backface SH flip), `rcgi_cache_shade`
(both-face lighting), a gather-variant pipeline or a push flag. **Risk**: the
pdf rebalance and the primary-surface thin detection are the parts most likely
to need reference-A/B tuning; validate in a Skyrim interior with barred windows
(their slide-91 scene) before enabling by default. Gate behind `RX_RCGI_THIN`.
