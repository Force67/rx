# Wave-driven shoreline wetting

## Goal

Darken and gloss the beach where the waves actually reach, and let it dry back
out over time once the swell recedes. The wet band follows individual waves up
the sand and lingers for a beat before fading, so a shoreline reads as alive
rather than a static texture seam. Opaque surfaces only; the water surface
shader is untouched.

## Field layout

`ShoreWetting` owns one camera-following, world-space `R16F` field
(`kResolution` = 1024², `kExtent` = 128 m, so ~0.125 m/texel) storing
`0 dry .. 1 soaked`. It is ping-ponged between two images that both stay in the
`GENERAL` layout: the compute pass reads last frame's field and writes this
frame's, and the opaque scene pass samples the one just written through env set
slot 33 (`kFrameFlagShoreWetting`; a 1×1 dry dummy binds when the feature is
off, so cost and shading are identical when disabled).

The field origin is snapped to the texel grid half an extent behind the eye so
it does not shimmer as the camera slides. Recentring is not toroidal: each frame
the compute pass resamples the previous field at the previous origin
(`prev_field`), which slides the history into the new frame in world space with
a single full-resolution pass.

`FrameGlobals::shore_field` carries `{origin.x, origin.z, 1/extent, unused}`;
`mesh.ps`/`mesh_rt.ps` map `world.xz` into `[0,1]` with it and sample the field.

## Per-frame update (`shore_wetting.cs.hlsl`)

For each texel at world XZ:

1. **Water height** — the same Gerstner field the water displaces with
   (`water_waves.hlsli`, so the wet line tracks the visible swell), or the FFT
   ocean displacement map when that path is active (`RX_FFT_OCEAN`). The
   Gerstner path ignores horizontal chop (a height-field approximation).
2. **Terrain height** — see the height source below.
3. `submerged = water >= terrain`.
4. History from the previous field decays by `exp(-dt / drying_time)`; the
   result is `max(submerged, decayed)`. Freshly reached texels snap to soaked;
   exposed ones dry exponentially (`shore_drying_time`, default 28 s).

## Height source and its limits

This PR uses an **analytic** terrain height: a radial gaussian dome
(`shore_island = {center.x, center.z, sigma, peak}`), evaluated identically by
`TerrainHeight` in the shader and by the island mesh built in
`CreateWaterDemoScene`. Keep the two in sync.

Limits, and why this is acceptable here:

- **One analytic beach.** The field only knows the single dome, not arbitrary
  world geometry. `TerrainHeight` is the sole coupling point: a **top-down
  orthographic depth capture** over the field extent can replace it (render the
  opaque scene from above into a height texture, sample it here) with no change
  to the rest of the pass. That is the intended production path.
- **Height-agnostic (2D).** Wetness is indexed by world XZ only, so a tall
  object standing in a wet texel reads as wet over its full height. Fine for
  ground/beach surfaces, which is the target.
- **Gerstner vs FFT.** When the FFT ocean is active the field samples its
  displacement map, so the wet line matches the rendered surface; otherwise the
  shared Gerstner field is used.

## Material response

Where wet (sampled in `ShadeSurface`, gated by the frame flag): `albedo *=
lerp(1, 0.55, wet)`, `roughness *= lerp(1, 0.45, wet)`, and a small
dielectric-`f0` lift (`lerp(f0, max(f0, 0.05), 0.6·wet)`) for the thin-film
sheen. The field's own bilinear filtering keeps the boundary a soft band rather
than a hard texel line. Applied to opaque draws; the water shader is not touched.

## Plumbing and rollout

- Settings: `shore_wetting` (bool, default off), `shore_drying_time` (s),
  `shore_island[4]` (analytic beach). Env `RX_SHORE_WETTING`, INI keys under
  `[water]`, and a debug-UI toggle + drying-time slider.
- The water demo turns it on and builds the matching sandy island. Other scenes
  leave it off.
- Graceful no-op: allocation/pipeline failure disables the feature (warn, not
  fatal); path-traced and interior frames skip it. `RX_SHORE_WETTING=0` restores
  identical shading for A/B captures.

## Validation

- Compiles to SPIR-V and DXIL through the existing shader build.
- GPU-verified in `--demo water` on real hardware: a dark wet band that follows
  the waves' reach up the beach, and recedes/fades over ~30 s of deterministic
  sim (`RX_FIXED_DT`, frame 60 vs frame 1800).
