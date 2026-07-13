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
