# Persistent water foam/ripple field

A camera-following, advected data field that gives the ocean surface *memory*:
foam that streaks and dissolves with the waves (and behind moving objects)
instead of the per-frame instantaneous crest term flickering in place, plus a
near-camera ripple wave equation for object interaction.

Files: `water_field.{h,cc}`, `shaders/geometry/water_field.cs.hlsl` (the update
passes), `shaders/geometry/water_field.hlsli` (the water-shader sampling side).
Setting `water_field` (default on) / env `RX_WATER_FIELD` / frame flag
`kFrameFlagWaterField`. Graceful no-op when allocation or the pipeline fails
(the water shader falls back to the instantaneous crest foam).

## Nested rings (clipmap-style storage)

`kRingCount = 2` rings (the code generalises to N), each a `kSize² = 512²`
`RGBA16F` texture, **ping-ponged** (two physical textures per ring). Extents:

| ring | world coverage | m / texel |
|------|----------------|-----------|
| 0    | 96 m           | 0.19      |
| 1    | 384 m          | 0.75      |

Channels: `R` ripple height, `G` ripple velocity, `B` foam density, `A` foam
age (seconds). Each ring's origin snaps to its own texel grid every frame, so
the field never swims under the camera. The rings live in `GENERAL` layout (like
the FFT ocean maps) and are sampled by the water pixel shader through env slots
30/31, with a params CB (origins + extents) in slot 32.

## Update passes (`water_field.cs.hlsl`, per ring, per frame)

Recorded into the render graph after the FFT ocean pass (so crest injection can
read the fresh foam map). Two dispatches per ring:

* **Phase 0 — recenter + advect + decay + ripple step.** Each output texel maps
  to a world XZ from the *new* snapped origin and resamples the *previous*
  texture (bilinear, keyed off the *old* origin). This full-res resample makes
  toroidal bookkeeping unnecessary. Foam is back-advected by the wave drift
  (`world − drift·dt`), decays exponentially (~9 s half-life) and accrues age.
  Ring 0 also steps a damped 2-D wave equation on `R`/`G` (neighbours read from
  the previous texture, so the write never races), with a stability clamp.
  Texels newly exposed at the edge read out of bounds → start empty.
* **Phase 1 — injection.** Crest foam: the FFT fold-foam channel (when the FFT
  ocean is on) plus a gentle analytic Gerstner-crest term, added above a low
  threshold so transient whitecaps get *stamped* into the field and then
  persist/advect. Object disturbances (below) add a wake ripple impulse (ring 0
  velocity) and a foam splat. Fresh foam is age 0; the running age is
  mass-weighted toward zero as foam is added.

All injections are per-second **rates × dt**; foam integrates over the ~13 s
foam time constant (density ≈ rate × 13), so small rates reach a modest steady
density instead of saturating.

## Object interaction (wakes)

`FrameView::water_disturbances` is a bounded `base::Vector<WaterDisturbance>`
(world pos, radius, ripple strength, foam amount, XZ velocity), uploaded into a
per-frame-in-flight storage buffer and consumed by the injection pass. In the
water demo each floating cube pushes one disturbance per frame scaled by its
physics velocity (derived from per-frame position deltas): horizontal + vertical
motion drives the ripple impulse and the foam splat, so a bobbing/drifting cube
throws foam and a still one leaves only a faint standing ripple.

Splash particles are intentionally *not* emitted — foam injection into the field
covers the wake more cheaply. (If particles were added they would follow the
additive convention: premultiplied fade into RGB, alpha = 1.)

## Local interaction (depth impulses + obstacles)

Setting `water_interaction` (default on, only meaningful with `water_field`) /
env `RX_WATER_INTERACTION` / debug-UI checkbox. Makes ANY geometry crossing the
surface ripple with no CPU disturbances, and makes ripples reflect off the beach
instead of passing through it. Both live inside the `water_field` pass's own
transient set (extra binding slots 4–7, all bound every dispatch; the depth
slot is a real graph resource, the rest are the field's own GENERAL images) plus
its push constant — no env-set slots, FrameGlobals fields, or frame flags were
added. **The pass is scheduled after the prepass** (renderer.cc, past the
`depth_export` write) so it can read the opaque depth; it still records after the
FFT ocean, so crest injection samples the fresh foam.

* **Depth-buffer ripples (ring 0 only, injection phase).** Each ring-0 texel's
  water column `(x, surface, z)` is projected into the frame with `view_proj`;
  the opaque prepass depth there is reconstructed to a world position with
  `inv_view_proj` (the exact reversed-z convention `contact_shadow.cs.hlsl`
  uses). Geometry that both straddles the waterline (`|geo.y − surface| < band`)
  and sits at this column (`|geo.xz − world| < proximity`, rejecting distant
  walls seen past the water) writes a soft **intersection band** into a small
  ping-ponged R32F mask (recentred with the rings via the same `PrevUv`
  resample). The band drives three things: a **bounded standing dent** the
  surface is *pinned* toward (`lerp` to a fixed target, so it can never
  accumulate — the previous agent's runaway came from integrating an un-signed
  source) which the wave equation rings outward; a **swell-driven bob** velocity
  (`mask × dSurface/dt`, zero-mean over a wave cycle) so a *static* floater keeps
  ringing as waves lap past it; and a **motion** velocity (`mask` change between
  frames) so a moving object rings harder. Foam is a `mask × swell-speed` RATE ×
  dt, so the long always-active shoreline reaches a modest steady density
  instead of saturating. The waterline `surface` rides the FFT swell (slot 7)
  when the ocean is on, an analytic proxy otherwise. Screen-space is
  view-dependent: off-screen / behind-camera texels **hold** their mask (no
  injection, no clearing), so scrolling the camera never flickers the field.
* **Obstacle boundaries (`water_interaction && shore_wetting`).** Texels whose
  analytic terrain height (the island push values, mirroring
  `shore_wetting.cs.hlsl`'s `TerrainHeight`) rises above the water are reflecting
  Neumann boundaries: their ripple height/velocity is zeroed every frame, and
  the phase-0 wave stencil substitutes the centre's own height for any neighbour
  that is an obstacle. Rings therefore bounce off the beach instead of leaking
  onto the island. Gated on `shore_wetting` because that is where the analytic
  island is a defined terrain source (a real world would swap in a heightmap).
  Debug-only `RX_WATER_OBSTACLE=0` forces just this off (leaving the wetting
  shading on) for an isolated A/B; it is intentionally not wired to the ini/UI.

## Shading (`water.ps.hlsl`)

The field is sampled once by world XZ with a ring-0-priority distance blend
(ring 0 inside its extent, fading to ring 1, then to zero past ring 1). Foam
uses a thickness model — `coverage = 1 − exp(−k·density)` — screen-composited
with the (now reduced-weight) instantaneous crest/shore term so the persistent
field always shows through in open water. Brightness is modulated by age (fresh
= bright white, old = grey), broken up by the existing high-frequency noise, and
thick foam reduces the fresnel/reflection response. Near the camera the ring-0
ripple-height gradient perturbs the shading normal (faded out with distance).

## Verification

`--demo water` sequence captures show foam streaks drifting coherently across
the open water (monotonic frame-gap divergence = persistence + advection, not
in-place flicker) and foam concentrating around the floaters. `RX_WATER_FIELD=0`
falls back cleanly to the instantaneous foam.
