# GPU-Based Procedural Placement

`engine/placement` populates the world with natural content (vegetation, rocks,
props, or any spatially distributed gameplay element) around the camera in real
time. Nothing is baked: the authored data is a compact world description plus
per-asset density rules, and the GPU reconstructs the surrounding point cloud
every time the camera moves into a new tile. The design follows the GPU-based
procedural placement technique presented at GDC 2017 (density fields + ordered
dithering).

## Core ideas

1. **Separate suitability from placement.** Authored logic never emits objects
   directly; it computes a continuous 2D *density field* per asset layer.
   A second, purely mechanical stage discretizes that field into points.
2. **World description, not object lists.** Input is a set of low-resolution
   2D maps (`WorldData`): heights, masks, topology of roads/water/objects,
   painted intent. A few MB/km² replaces millions of stored transforms.
3. **Determinism + local stability.** The same inputs always produce the same
   placements, and an edit to one map region only changes placements that
   sample that region. Tile + pattern-point index form a stable identity that
   seeds all per-instance randomness.
4. **Footprint-scaled granularity.** Every layer has a footprint (effective
   diameter). Tile world size and dither-pattern scale derive from it, so
   trees are evaluated on large coarse tiles and grass on small dense ones,
   with a fixed 64×64 density resolution either way.
5. **Collision by construction.** Layers that share a footprint are stacked
   into one dither interval (*layered dithering*): the threshold test selects
   at most one layer per sample point, so same-footprint layers can never
   overlap — no readback, no rejection pass.

## Pipeline

Per active layer-tile, three GPU stages:

```
WorldData maps ──► DENSITYMAP ──► GENERATE ──► PLACEMENT ──► instances
 (R8/RG8 2D)      64×64 density   ordered      transforms +   (instanced
  streamed         texture per    dither →     AABBs, stable   draw / ECS)
  around camera    tile           oriented     random per ID
                                  points
```

- **DENSITYMAP** — evaluates the layer's compiled density program over one
  world-space tile into a 64×64 density texture. Programs are a small
  stack-based bytecode (sample map, curve/remap, min/max/mul/add, constants,
  noise), compiled from the authored layer description. Layers in a stack
  accumulate: each writes `[lower, lower+density)` intervals for layered
  dithering.
- **GENERATE** — one thread per pattern point: reject outside tile → threshold
  test against density (interval test for stacked layers) → place 2D point →
  sample height map for 3D position → derive normal → stage in groupshared →
  one atomic append per group. Output: oriented points + `{tile, point index}`
  identity.
- **PLACEMENT** — turns each oriented point into a final world transform +
  bounds: deterministic hash of the stable identity drives rotation, uniform
  scale, tilt-to-normal blend, elevation offset per the layer's variation
  parameters.

The dither pattern is a checked-in, deterministically generated set of 256
unit-tile positions ordered so that any prefix is evenly spaced (greedy
farthest-point ordering, toroidal metric; see `engine/placement/PATTERN.md`).
Pattern rank defines the threshold: `t(i) = (i + 0.5) / 256`. A per-layer
noise offset breaks up residual tiling structure.

## Authoring model

- **Ecotope** — a reusable environment description: a list of *layers*
  (asset + footprint + density program + variation params), grouped so a rule
  applied to a group modulates all children. Ecotopes are placed into the
  world via an ecotope-mask WorldData map and can blend.
- **WorldData** — named 2D float/vec maps registered with a world-space
  origin, extent, and resolution; mipmapped so any layer granularity samples
  an appropriate LOD. Sources: procedural generation, imports, artist
  painting. The density bytecode addresses maps by slot.
- The authored form compiles to a flat array of runtime layers; same-footprint
  layers within an ecotope are stacked (most common layers lowest, so the
  interval test can early-out cheapest).

## Scheduling

Tiles stream in rings around the camera per footprint class (e.g. trees
128 m tiles / radius 400 m; undergrowth 32 m tiles / radius 96 m). Per frame
the scheduler batches by stage across all dirty tiles — all DENSITYMAP
dispatches, then GENERATE + allocation, then PLACEMENT into one shared output
buffer, then a single copy/consume — rather than round-tripping per tile, and
caps emits per frame to bound memory and avoid GPU spikes. Evicted tiles free
their instance ranges; re-entering a tile reproduces byte-identical output.

## Editing

WorldData maps are mutable at runtime (brush writes); a dirty region
invalidates only intersecting tiles, which regenerate next frame. Because
identity is positional, untouched tiles keep identical instances — a local
edit produces a local response.

## Layout

- `engine/placement/` — `rx::placement`: WorldData, density programs,
  ecotopes/layers/stacks, tile streaming, CPU reference generator
  (headless-safe, links only `rx::core`); `rx::placement_gpu`: the three
  compute pipelines + batched scheduler (`gpu_placement.{h,cc}`,
  `shaders/placement_{density,generate,transform}.cs.hlsl`).
- `engine/placement/placement_pattern.h` — checked-in generated dither
  pattern; `pattern_gen.cc` + `PATTERN.md` document regeneration and quality.
- `runtime/demo_placement.{h,cc}` + `runtime/placement_demo_assets.{h,cc}` —
  the `--demo placement` scene and its hand-authored low-poly nature meshes.

## Demo

```sh
vkrun ./build/linux/runtime/rx --demo placement
```

A 1 km² landscape (height/forest/road/water WorldData + one forest ecotope:
pine, broadleaf, dead tree, bush, rock, grass) dressed entirely by the
pipeline — ~10k instances live around the camera, nothing stored. Fly around:
tiles stream in and out; leaving and returning reproduces every instance.
`RX_PLACEMENT_FLY=1` autopilots the camera along the road,
`RX_PLACEMENT_LINES=1` overlays streaming tile bounds.

## Testing

- `placement_test` (CPU): pattern guarantees, density-program evaluation,
  determinism, footprint spacing, layered-dither exclusivity, road masking,
  local stability under a painted edit, streaming-ring bookkeeping.
- `placement_gpu_test` (offscreen, skips without a driver; run under
  `vkrun`): the full GPU pipeline against the CPU reference — identical
  instance sets, bit-identical GPU reruns.
