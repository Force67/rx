# Ship demo (`--demo ship`)

A procedural sailing-ship slice on the adaptive-water / FFT-ocean stack: a lofted
wooden brig with wind-billowed sails, verlet rope rigging and timed cannon
broadsides, cruising past a second anchored vessel. It is **content only** — it
drives the ship through the same public physics/render APIs the water demo uses
(`AddDynamicBox` + the buoyancy callback, `WaterDisturbance` wakes, additive demo
particles) and touches no engine internals, water shaders, or `FrameGlobals`.

All the code lives in `runtime/demo_ship.{h,cc}`. The registry wiring is three
lines: a `ship_` member on `DemoScenes`, a dispatch arm in `CreateDemoScene`, and
one `ship_->Emit(...)` call in `EmitToView`.

## What it shows

| Feature | How |
| --- | --- |
| **Hull** | A wooden hull lofted from a U-profile swept along the keel: `width_scale` tapers it to a fine stem at the bow and a fuller transom aft, `sheer` lifts the deck fore/aft. A recessed flat deck caps the top; a transom fan closes the stern. One single-material static mesh → `asset::GenerateLods` gives it distance LODs. |
| **Rig** | Masts, yards and bowsprit are baked cylinders in one wood mesh (`AddCylinder`). |
| **Sails** | Billowed square grids, curvature baked along the wind direction (silhouette change) + `Material::wind` so `mesh.vs` flutters the free edge (uv.y weight, capped at 0.6 so the strong default wind does not fold the canvas ragged). Rendered in the opaque path → they cast shadows. **No per-frame re-upload**: the animation is free in the vertex shader. |
| **Floating + wake** | The hull is a Jolt dynamic box floating on `set_water_height`; a gentle centre-of-mass impulse each frame holds a slow cruise (no torque → straight heading). `EmitWake` injects bow/stern/side/trailing `WaterDisturbance`s scaled by speed, exactly like the water-demo cubes, so the `WaterField` draws the foam trail. |
| **Ropes** | A handful of stays/shrouds/sheets as CPU verlet strands (≤14 segments), re-simulated each frame under gravity + a swaying local wind, drawn as thin ribbons. See the pitfall below. |
| **Cannons** | `N` gunports per side; a timer fires alternating broadsides: additive muzzle flash + grey powder smoke (demo particles), a brief warm muzzle point-light, and an iron cannonball mesh on a ballistic arc. On surface impact it injects a strong `WaterDisturbance` (ripple + a large one-shot foam patch that lingers in the field) plus a white spray burst. |
| **Second vessel** | The anchored ship reuses the flagship meshes at a fixed pose — free multi-vessel + hull-LOD coverage. |

Additive particle convention (matches the water demo / `particle_emitters.cc`):
the life fade is premultiplied into the HDR RGB radiance and alpha stays 1, so a
particle never death-pops. `view.particles_emissive = true` routes the whole set
through additive blending — one flag for the set, so the powder smoke is authored
as dim grey additive rather than lit alpha.

## Pitfall: per-frame rope re-upload

`Renderer::UploadMesh` calls `device_->WaitIdle()` when re-uploading under an
existing key ("uploads happen at load time; never per frame"). The rope ribbon is
the one mesh that changes every frame, so re-uploading it is a deliberate
per-frame GPU stall. It is kept tiny (a few short strands) for that reason. A
proper dynamic vertex-buffer / transient-geometry path in the renderer would
remove the stall; until then this is the only way to draw re-simulated geometry.
Sails deliberately avoid this by animating in `mesh.vs` instead.

## Hook for the water-bodies agent

The flagship floats on a flat `set_water_height` (height 0) and emits wakes
through `FrameView::water_disturbances`. When the parallel swell-riding buoyancy
lands, the ship's dynamic box will ride the swell with no demo change (it already
reads its pose back from Jolt each frame in `UpdateShipMotion`). Velocity-shaped
directional wakes / slam splashes will flow straight into the existing `EmitWake`
and cannon-impact disturbances — those already pass `velocity_x/z` and speed-scaled
`foam_amount`, so the improved field just needs to consume them.

## Crew (stretch) — skipped

The locomotion demo's animated biped needs a per-character `RigPlayer` +
`FootPlacement` + skin-palette pipeline (~70 lines each) plus a ground to foot-IK
against; transplanting 2–3 onto a moving, bobbing deck (feet would need to ride
the hull transform, not a static floor) exceeded the "cheap, <100 lines" bar, so
crew were left out.

## Verify

```
DISPLAY=:10 RX_UI_SHOT=/tmp/shot.png RX_UI_SHOT_FRAMES=130 RX_FIXED_DT=0.0166667 \
  nix develop -c vkrun stdbuf -oL ./build/linux/runtime/rx --demo ship
```

Broadsides fire at t≈2.5 s then every 5 s (frames 150, 450, …); the first fires
toward the chase camera so its splashes land in the foreground. The chase camera
follows the flagship unless the player supplies look/move input.
