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
