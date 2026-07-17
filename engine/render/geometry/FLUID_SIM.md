# Heightfield fluid dynamics (water + lava)

An **optional** GPU shallow-water solver (`fluid_sim`, default **off**, env
`RX_FLUID_SIM`) that makes standing water a *real* fluid over a world region:
water flows downhill, fills basins, floods through gaps when a hindrance is
removed, and settles level. Lava is the same solver with a temperature field:
it creeps (temperature-dependent viscosity), cools, and **solidifies into new
terrain**.

This is the classic *virtual pipes* heightfield scheme (O'Brien & Hodgins 1995;
Mei, Decaudin, Hu 2007 "Fast Hydraulic Erosion Simulation on GPU"), the family
of solvers behind From Dust's water/lava and most large-scale game fluids. A
heightfield sim is the right tool at this scale: a 512² world-anchored grid
covers a whole play space for microseconds of GPU time, where particle methods
(FLIP/PBF) blow the budget at three orders of magnitude less coverage.

## Solver

Per cell (grid spacing `l`): static bed `B` (terrain + stamped obstacles),
solidified crust `C` (lava deposits, grows at runtime), fluid depth `d`,
total surface `h = B + C + d`, outflow flux `F = (fL, fR, fT, fB)` through the
four virtual pipes, temperature `T` (lava).

Per substep (fixed `dt`, CFL-clamped, N substeps/frame):

1. **Flux pass** — for each direction with surface difference
   `Δh = h(cell) − h(neighbour)` (for lava reduced by the yield head, below):
   `f' = max(0, (f + dt · g · (A/l) · m(T) · Δh) / (1 + k_drag(T)·dt))`
   with pipe cross-section `A = l·l`, a mobility `m(T) ≤ 1` and implicit drag
   `k_drag(T)` (both constant for water, temperature-driven for lava). Then the
   volume clamp (Mei et al.): `K = min(1, d·l² / (Σf' · dt))`, `f' *= K`, so a
   cell can never export more volume than it holds — this keeps the explicit
   scheme non-negative and volume-conserving through dam-breaks.
2. **Depth pass** — pure gather; mass conserved by construction:
   `d' = d + dt/l² · (Σ inflow − Σ outflow)`, inflow read from the four
   neighbours' outflow fluxes. Cell velocity is derived from through-flux:
   `u = (fR(x−1) − fL + fR − fL(x+1)) / (2·l·max(d̄, ε))` with
   `d̄ = (d + d')/2` (likewise `v`), stored for advection, flow-foam and
   normal shading, and clamped to `0.5·l/dt` (Chentanez & Müller's velocity
   clamp — the single most effective guard for violent scenes).
3. **Thermal pass (lava only)** — heat is advected *with the mass fluxes*
   (donor-cell: each outflow carries the donor's temperature, so merging flows
   mix conservatively), then cooling and solidification (below).

Boundary cells reflect (their off-grid flux component is zeroed in-shader).
Cells whose bed rises above the fluid surface need no special casing — the
wetting/drying front falls out of the volume clamp. A mild edge-overshoot damp
(Chentanez & Müller) suppresses the ringing a steep dam-break front leaves
behind.

**Stability**: gravity waves travel at `√(g·d)`, so the explicit scheme wants
`dt ≤ ~0.3 · l / (|u|max + √(g·d_max))`. The sim runs a FIXED substep (1/120 s,
per-frame cap, remainder carried) — deterministic and frame-rate independent —
which sits inside that bound for the default grid (0.25 m cells, a few metres
of head). There is no runtime CFL clamp: much finer cells or much deeper water
need a smaller `kSubstepDt`; the volume clamp, velocity clamp and drag keep
out-of-bound configurations non-exploding (ringing, not NaNs).

## Lava

The formulation follows MAGFLOW (the INGV Etna cellular-automaton model) and
From Dust's "same solver, different viscosity per material" philosophy:

* **Viscosity → mobility**: Arrhenius-style
  `m(T) = 1 / (1 + η₀·exp(−k·(T − T_liq)))` — hot lava flows like thick water,
  cooling lava creeps exponentially slower.
* **Yield stress → critical slope** (Bingham fluid): outflow only sees the
  head *above* a temperature-dependent yield threshold,
  `Δh_eff = max(0, Δh − h_yield(T))` with `h_yield` rising as `T` falls. This
  is what makes real lava fronts *stop on a slope* and pile into lobes instead
  of thinning forever — pure viscous damping cannot produce that behaviour.
* **Cooling**: radiative-dominated relaxation toward ambient scaled by
  `1/max(d, d_ref)` — thin flows and front edges cool fast, thick ponds stay
  hot (the physically correct `T⁴` law collapses to this over a lava's active
  range).
* **Solidification**: below `T_sol`, depth transfers into the crust layer at
  `C += min(d, r_sol·dt)`, `d −= …` (gradual, so fronts freeze into rounded
  toes). Crust is part of the bed for *both* fluids from then on — lava
  genuinely builds new terrain that later water (and lava) flows around.

## Interaction API (how "hindrances" work)

The game (or demo) drives the sim through the frame:

* **Obstacles** are boxes stamped *into the bed*: adding one raises `B` over
  its footprint, removing one restores the terrain underneath — the water that
  was held back floods through on the next substeps. That is the core
  "remove the hindrance and the water flows" mechanic.
* **Sources/sinks**: bounded per-frame list (position, radius, rate in metres
  of depth per second within the radius, fluid type, temperature) — springs,
  vents, drains.
* **Region fill**: initial-condition helper to pre-fill a basin/reservoir.

One domain is active at a time in v1 (world-anchored origin/extent/resolution;
default 512² — the architecture generalises to multiple domains later). The
**bed is CPU-authoritative**: the game owns terrain + obstacle heights and
(re)uploads the bed texture when they change (a 512² R32F upload is ~1 MB —
trivial for discrete events like a dam opening). The two fluids share the
domain: lava solves first each substep, then water sees `B + C + d_lava` as
its bed; where hot lava and water meet, the lava quenches to crust and some
water boils off.

## Rendering

A dedicated displaced-grid surface draw over the domain (transparent phase):
the VS samples `B + C + d`, the PS shades from the height-gradient normal plus:

* **Water**: depth-tinted absorption, IBL fresnel reflection, and *flow foam* —
  velocity-advected detail (two phase-offset flow-map samples) with foam keyed
  to speed, so rapids and the dam-break front read as churning water.
* **Lava**: blackbody emissive from `T` (hot core → glowing cracks → dark
  crust), crust albedo where solidified, slow flow-mapped surface detail.

Cells with `d` below a small threshold fade/discard, so the surface only exists
where fluid is. The FFT ocean / WaterField stack is untouched — this module is
for *bounded dynamic* water (floods, pools, channels, lava), not the ocean.
The surface draws inside the transparent pass, which currently exists only when
the ray-query water pipeline is available — on a non-RT device the whole
feature (solver included) stays inactive rather than simulating invisibly.

## Verification

`--demo fluid`: a terraced basin with a reservoir held by a dam of obstacle
boxes; the dam breaks on a timer (10 s, or frame `RX_FLUID_DAM_FRAME` for
deterministic captures with `RX_FIXED_DT`) and the flood pours through the
notch, fills the lower basin and settles level (volume conserved). A lava vent
on the hill feeds a pond that spreads, cools and solidifies into a crust ring.
`test/fluid_sim_test` runs a dam-break on a 64² bowl and asserts exact volume
conservation, no NaNs/negatives, and that the flood settles level after the
ridge is removed (a `bed_version` re-upload). `fluid_sim` off (default) leaves
every existing path untouched.
