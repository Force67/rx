# Cloudscape

An opt-in weather model: an art-directable weather-state layer
(`rx::weather::WeatherSystem`) loosely coupled to a textured volumetric cloud
renderer (`render::Cloudscape`) through one compact control struct. The weather
side decides *what atmospheric conditions exist and where*; the renderer turns
those controls into density and light. Neither knows the other's internals,
which is what keeps the system procedural, region-aware, artist-drivable and
cheap at the same time.

Enable with `RenderSettings::cloudscape` (`RX_CLOUDSCAPE=1`). When on, it
replaces the always-on procedural `clouds` pass. `--demo sky` is the reference
scene.

## The seam: `CloudscapeControls`

`engine/render/atmosphere/cloudscape_types.h`. A weather system (or the app
directly) writes `settings().cloudscape_controls` each frame:

| Field | Meaning |
| --- | --- |
| `map_a`, `map_b`, `map_blend` | two weather-map states (seed, coverage, cloud type, precipitation) cross-fading during a transition |
| `map_offset` | integrated wind advection of the map domain (metres) |
| `wind_yaw`, `wind_speed`, `vertical_skew` | density-field advection; tops lead bases by the skew |
| `turbulence` | curl-noise distortion of the erosion detail |
| `density`, `bottom`, `top` | global density and the shell altitudes |
| `anvil` | storminess: flat spread tops + harder sun absorption |

## The weather layer (`engine/weather/`)

- **States** package a full cloudscape + precipitation + wind + lightning
  configuration with scheduling weights, day/night bias, dwell and transition
  times.
- **Regions** (XZ boxes with priority, plus a global fallback) restrict which
  states may be scheduled where, so a desert and a coast draw from different
  distributions.
- **The scheduler** picks the next state by weighted random (seeded,
  deterministic) when the current dwell expires; **transitions** lerp every
  scalar and cross-fade the weather map rather than snapping.
- **Scripted override**: `ForceState(index, seconds)` bypasses scheduling for
  story moments and holds until `ClearForced()`.
- The layer also integrates what the renderer's `WeatherSettings` expects the
  game to own: surface wetness soak/dry, snow build/melt, and stochastic
  lightning strikes while a storm state is active.

## The cloud model

Density is a pure function of position — no voxel storage. Per sample:

1. **Shell height**: normalized altitude inside a spherical layer
   (base/top from the controls) so the deck drops correctly to the horizon.
2. **Weather map** (512², world-tiling, GPU-regenerated only when its inputs
   change): R coverage, G precipitation, B cloud type, sampled by world XZ.
3. **Height profile**: stratus / stratocumulus / cumulus vertical envelopes
   blended by cloud type; storminess lets tops spread into an anvil.
4. **Base shape**: 128³ tileable RGBA8 texture — a Perlin-dilated-by-Worley
   channel plus three Worley octaves — thresholded by coverage as a *remap*,
   so rising coverage grows and joins formations instead of fading a sheet in.
5. **Erosion**: 32³ Worley detail, its lookup swirled by tileable 2D curl
   noise, subtracted at the cloud boundary (silhouette survives, edges shred);
   the detail flips near the base for foggy undersides.
6. **Wind**: the whole field advects downwind; vertical skew shears tops.
7. **Virga**: below precipitating cells the marched interval extends under
   the base, where vertically stretched noise hangs rain curtains that
   evaporate before the ground.

## The renderer

Half-resolution persistent buffer; each frame only 1 pixel of every 4×4 block
marches fresh (16-frame refresh cycle), the rest reproject last frame through
the stored per-pixel cloud distance (two-projection parallax). Disocclusions
and history misses fall back to a fresh march. A full-res pass composites the
buffer over the lit scene and conservatively checks cloud distance against
full-resolution scene depth, so filtering cannot bleed cloud onto foreground
geometry.

The march itself:

- **Two-mode walk**: cheap base-texture taps stride 3× wide through empty air;
  on contact the walk backs up one stride and switches to full-detail samples,
  returning to cheap mode after 6 consecutive empty taps.
- **Adaptive counts**: `cloudscape_steps` potential full samples toward the
  zenith (clamped to 8–128), 2× toward the horizon, early exit at 99% opacity.
- **Lighting**: 6-tap cone toward the sun (5 near, 1 far for distant tower
  shadows), Beer extinction with a two-scale floor standing in for multiple
  scattering, a two-lobe phase (forward silver lining + back lobe), a powder
  term for sugary edge detail, and precipitation-scaled absorption so rain
  cores go graphite. Ambient grades with height and the deck flashes from
  within during lightning.
- A one-tap high wisp sheet above the shell keeps clear skies from reading
  empty.

Cloud sun-shadowing on the ground reuses the existing `cloud_shadow` layer,
fed the blended map coverage.

## Vertical structure

Shell altitudes are authored **per weather state** (`WeatherState::base_altitude`
/ `top_altitude`, cross-faded through transitions like every other control) and
follow the real vertical structure of each cloud class:

| Class | Base | Top |
| --- | --- | --- |
| stratus | near-surface .. ~1.2 km | thin, a few hundred metres |
| stratocumulus | ~0.6–1.5 km | ~2.5 km |
| cumulus | ~0.5–2 km (to ~2.7 km in dry climates) | fair-weather +1–3 km |
| nimbostratus (rain deck) | low base | body 2–7 km (temperate) |
| cumulonimbus | ~0.3–2 km | 10–16 km anvil |

So the demo's overcast is a genuinely low thin ceiling, the fair-weather states
ride a high dry-climate condensation level, and the storm is a tower running
from 1.5 km to a 12 km anvil.

`CloudscapeControls::darkness` (from `WeatherState::darkness`) is the menace
knob: 0 leaves physics alone, 1 collapses the ambient and multiple-scatter
floors while sun absorption climbs, so severe-storm skies go genuinely black
instead of merely grey.

## Ground haze

`fog_density` / `fog_height` (per weather state, blended like everything
else) drive an analytic exponential height-fog layer that shares the deck's
weather instead of being a flat screen effect: the optical depth is
closed-form (no marching), then the weather map's humid cells fog up harder,
two wind-advected taps of the baked base noise drift mist banks through it,
the in-scatter uses the same sun-elevation ambient the clouds use (evening
haze glows warm), the deck's coverage shades the fog beneath it, menace murks
it, and the lightning flash breathes through it. The weather layer also
raises fog automatically while the ground is still wet after rain stops —
post-rain mist that fades as the surface dries. `fog_churn` scrolls the banks'
noise vertically so the layer roils like rising vapour (high over warm
standing water, low for a settled morning layer). Composited after the cloud
pass so the nearest medium correctly veils both the scene and the distant
deck. The sun in-scatter is marched with one cheap deck-density tap per
segment, so fog in a cloud gap glows while fog under a core sits in a shadow
shaft (god rays that track the actual formations), and the sun colour runs
through the sky's transmittance LUT — the fog reddens with exactly the
atmosphere the sky renders. The fog floor follows the terrain via the same
ground sampler the lightning uses (`WeatherSystem::SetGroundHeight`).

## Knobs

- `RX_CLOUDSCAPE=1` — enable the model (or `cloudscape = true` in render.ini).
- `RenderSettings::cloudscape_steps` — quality lever (default 48).
- `--demo sky` + `RX_SKY_STATE=<n>` — reference scene, optionally pinned to
  one weather state (0 clear, 1 scattered, 2 overcast, 3 storm).
- `RX_SKY_FAST=1` — short dwell/transitions for watching the scheduler work.
- `RX_SKY_FOG=<0..1>` / `RX_SKY_FOG_H=<m>` — override the pinned state's
  ground haze (e.g. `RX_SKY_FOG=0.6 RX_SKY_FOG_H=14` for knee-deep mist).
- `RX_SKY_CAM_Y=<m>` — start the camera above the mist blanket, looking down.
- `--demo swamp` — fog showcase: dead snags and mossy hummocks on wet mud
  under a forced low-stratus state with thick shallow mist and puddle sheen.

## Tornado

States opt in via `WeatherState::tornado_prone`; while the blended deck is
anvil-heavy the layer runs a full vortex lifecycle — touchdown upwind of the
player, a snaking downwind wander, rope-out — writing
`CloudscapeControls::tornado_pos/strength/radius`. The renderer marches a
small bounded funnel volume between ground and cloud base: a hollow noise
cone whose axis snakes with height, whose streaks rotate around the axis, and
whose contact point churns a dust skirt. Rays that miss the funnel's bounding
cylinder exit after two dot products, so the pass costs nothing without a
funnel and very little with one. `--demo sky` state 5 (`RX_SKY_STATE=5`) is a
supercell that spawns them; `RX_SKY_TRACK=1` keeps the camera aimed at the
funnel (its spawn ring is player-relative and random).

## Thunder

`audio::MakeThunder` (engine/audio/thunder_synth.h) renders a one-shot
procedural clap: a band-passed crack transient into a brown-noise rumble that
rolls through seeded echo bumps. Distance shapes it like air does — the crack
dies within ~1 km and the rumble's lowpass closes down, so far strikes arrive
as a deep muffled roll. The sky demo queues one per strike, delayed by the
speed of sound and placed at the channel, so the flash always leads the sound.
