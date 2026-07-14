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
- **Blend fallback** on a cache miss (hash overflow / eviction) is black rather
  than a probe/sky fallback; rare in practice, mild darkening only.
- **Transparent surfaces** get no RCGI in M1 (only the opaque scene env-set is
  wired); DDGI wired transparents, so this is a small gap under RCGI. Easy
  follow-up (add the trailing arg to the transparent `WriteEnvSet`).
- Reflections/spec-refl bounce still reads the DDGI atlas (black under RCGI);
  not wired to the cache yet.

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

- **Reflection/spec-refl bounce** still reads the DDGI atlas (black under RCGI) —
  unchanged from M1, not wired to the cache/gather yet.
- **Quarter-res gather** is a one-constant change (`kGatherDivisor = 4`) but not
  exposed as a user setting; not yet tuned/verified.
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
