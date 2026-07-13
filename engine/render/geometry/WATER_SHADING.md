# Physically-based water shading

Three cooperating pieces upgrade the water surface and the surfaces beneath it.
All are tunable through the `[water]` block of `RenderSettings`
(`settings.h` / `settings_ini.cc`) and the **Water shading** section of the debug
UI, and each has a graceful no-op fallback.

The tunables ride in `FrameGlobals` as three trailing `float4`s (appended last,
mirrored in `mesh.ps.hlsl`, `mesh_rt.ps.hlsl` and `water.ps.hlsl`):

| field | xyzw |
|-------|------|
| `water_absorption` | rgb Beer–Lambert coefficient (1/m), w overall scale |
| `water_material`   | x transmission, y reflection foam gain, z crest-SSS intensity, w crest-SSS exponent |
| `water_caustics`   | x caustic intensity (0..1), y water rest height (m), z extra depth-fade (1/m), w unused |

## 1. Material (water.ps.hlsl)

**Absorption.** The refracted path behind the surface is attenuated by
Beer–Lambert over `water_depth` (reconstructed from the opaque depth):
`transmittance = exp(-absorption * water_depth)`. Per-channel coefficients make
red die first, so deep water turns blue-green while shallow water stays clear.
Verified A/B: absorption off → neutral grey water (B/R≈1.04); on → B/R climbs
1.3→1.7 with depth.

**Transmission.** `water_material.x` scales how much of the refracted floor
survives versus the scattered body colour; foam reduces it further downstream.

**Reflection roughening.** Foam density (`1 - exp(-1.1·foam)`) plus ripple energy
drive `refl_rough`, which blends the sharp RT/mirror reflection toward a blurred
sky mip and dims it — foamy/choppy water reads matte instead of glassy.
`water_material.y` is the gain.

## 2. Wave subsurface-scattering crest glow (water.ps.hlsl)

Backlit sun transmits through thin, lifted crests toward the camera. A thickness
proxy is built from the wave state — `thickness = lerp(1.4, 0.10, crest) +
saturate(-height_above)*0.8` — so pinched crests and lifted water are thin,
troughs are thick. Thickness becomes attenuation through
`exp(-thickness · absorption · 4)`: thin crests transmit nearly white, thick
bases go dark and turquoise (red absorbed first). The lobe peaks where the view
aligns with the **refracted** sun travel direction
`refract(sun_travel, n, 1/1.33)` (the transmitted light heads toward the
camera). Intensity/exponent are `water_material.z/.w`; the whole block is skipped
when intensity is 0 (zero cost). The colour is derived from the absorption, not a
separate tint. Verified A/B under a grazing sun: the term adds a turquoise band
(green/blue boosted, red suppressed) across the backlit water.

## 3. Underwater caustics + wave shadows (water_caustics.{h,cc,cs.hlsl})

A compute pass (`WaterCaustics`) rebuilt fully each frame writes a **tiling
world-space RG16F map** (env slot 34, `kTile = 64 m`, matching the FFT patch so
the FFT path tiles seamlessly):

- **R = energy-conserving caustic density.** A 512² grid of surface photons is
  refracted through the surface — the coarse Gerstner/FFT normal is refined with
  fine animated capillary ripples, since it is the sub-metre ripples (not the
  swell) that focus the sun into a caustic web over a shallow receiver. Each
  photon lands on a reference receiver plane a fixed depth below rest and is
  **bilinearly splatted with unit energy** into the accumulation buffer
  (fixed-point `InterlockedAdd`). Because photon count == texel count and each
  deposits exactly one unit, the map's **mean is 1**: convergent refraction piles
  photons up (R>1, brighter) and divergent refraction thins them out (R<1,
  darker) — no free energy. Three phases: clear → scatter → resolve.
- **G = wave shadow.** The sun's Fresnel transmission through the surface above
  the texel; the backs of waves let less light through.

`mesh.ps`/`mesh_rt.ps` sample the map (`water_caustics.hlsli`, gated by
`kFrameFlagWaterCaustics`, bit `1<<15`) for surfaces **below** the rest height and
fold the result into the sun `shadow` term, so it modulates direct sun only. The
mean-1 pattern is faded toward 1 with depth (luma-weighted absorption +
`water_caustics.z`), keeping deep water energy-neutral. Because the modulation is
mean-1, it both brightens and darkens without changing average irradiance.

Verified A/B on the demo island's submerged sand skirt: caustics both brighten
and darken around mean 1 (balanced pixel counts), and the pattern fully
decorrelates over ~1 s (it animates).

### Gating & kill switch

`water_caustics_active_` requires a water surface in the scene (FFT ocean or the
foam field active), not path-traced, not interior. `RX_WATER_CAUSTICS=0` (or the
`water_caustics` setting) forces the pass off; the shader then returns 1 and the
prior look is restored exactly. The other tunables are settings/UI only.

**Pitfall / gap for the integrator:** the receiver plane is a single reference
depth, and the "surface below rest height" test uses one global `water_rest_height`.
Scenes with unrelated geometry below that plane would receive spurious modulation,
so caustics are gated on water presence; a proper per-region water height/mask
would refine this. The caustic contrast also depends on the sun being a
meaningful fraction of the surface's lighting — under a sky-ambient-dominated
demo it is real but subtle (measure, don't eyeball).
