# Authoring scenes as text

`.rxscene` is a text scene format meant to be written directly, by a person or
by an agent, and verified without opening a window. Everything below is
reachable from a shell.

Worked examples, smallest first: `runtime/scenes/cornell.rxscene`,
`showcase.rxscene`, `model.rxscene`, `gallery.rxscene`, `city.rxscene`.

## The loop

```sh
rx --dump-schema                      # every component and prop, generated from reflection
rx --dump-materials                   # the named material palette
rx --validate scene.rxscene           # structural check, no GPU, exits nonzero on error
rx --scene scene.rxscene --headless --shot out.png --shot-frames 20 \
   --width 1280 --height 720          # render it, exits nonzero if no png was written
rxdiff before.png after.png           # did it change more than the renderer's own noise
```

Author, validate, render, look at the png, fix. `--dump-schema` and
`--dump-materials` are generated, never hand-maintained, so they cannot go
stale. Read them rather than guessing prop names.

## Composition, in order of how much hand-written arithmetic they remove

- **`Prefab.path`** instances another `.rxscene`. Its first entity's components
  land on the instancing entity; components the instance already declares win.
  Merge is **per component**, so an instance can override a whole `Surface` or
  `Pattern` without restating geometry, but declaring any part of `Shape`
  replaces the prefab's `Shape` entirely, which is why proportions live in
  `Stretch` (below) rather than in `Shape`.
- **The material palette** (`runtime/scenes/materials/`) is 30 named presets,
  instanced exactly like any other prefab. Prefer `Prefab.path =
  "materials/steel.rxscene"` over guessing PBR floats.
- **`Anchor.target` / `Anchor.mode`** places an entity against another's built
  geometry (`on | under | left | right | front | behind`), so no height or
  offset is ever written down. It measures the *rotated, stretched* bounds, so
  a turned object sits on its corner correctly.
- **`Grid`** lays declared members out on a lattice. `Grid.cell` gives every
  member that does not name its own prefab one to instance.
- **`Rotation.euler`** is degrees, yaw then pitch then roll about the entity's
  own axes.
- **`Stretch.scale`** is a per-axis scale baked into the entity's built `Shape`,
  on top of `Shape.size`. It is a component of its own precisely so it composes
  with a prefab: one building prefab instanced at three proportions is three
  `Stretch.scale` lines, no restating of `Shape.kind` or `Shape.size`. Every
  axis has to be positive.

Anchors and grids both **replace** `Transform.position`, so an entity cannot use
both, and grid members all share the grid's y. Buildings of differing height are
placed individually rather than gridded, for that reason.

## Failure modes worth knowing

- A malformed number, an unknown component or prop, an unknown `Shape.kind`, a
  zero quaternion and a dangling `Anchor.target` all **fail the load** with a
  `path:line:` message. They are never silently substituted.
- `--validate` reports every finding in one pass, including things a strict load
  cannot see (non-finite literals, degenerate scale, duplicate Guids, a
  `Renderable` with no `Transform`, anchor cycles).
- `Pattern.scale` is in **uv space, not world space**. The same pattern on a
  bigger box gives bigger cells, and a non-cubic box stretches it per face.
  Retune `scale` per prefab rather than expecting a fixed texel density.
- `Prefab.path` resolves **relative to the file naming it**; `Model.path` and
  `Surface.materialx` resolve relative to the **working directory**.

## Running it

The binary needs the nix dev shell at runtime, so prefix everything with
`nix develop -c`, including `--validate` and `--dump-schema`.

For anything that renders, wrap in `vkrun` (real GPU) or `swrun` (software).
**A run with neither has no Vulkan loader, silently falls back to a stub that
writes no png, and exits nonzero.** Under `swrun` add `--no-rt` or
`--preset low`: lavapipe's acceleration-structure builds crash on their own.

## Comparing renders

Captures are deterministic: `--shot` implies a lockstep clock, so the same scene
and build reproduce to about rmse 0.0002. `rxdiff` defaults to a 0.002 limit
over a measured 0.00043 floor, which catches a 12% albedo change.

- **Diff at 20+ frames.** At 8 frames a busy scene's own run-to-run noise can
  exceed the tolerance and fail against itself.
- Never compare by hash. Deterministic is not bit-identical; a residual remains
  from the radiance cache's atomic claim order.
- **GPU-backed tests skip with exit 0 when there is no Vulkan loader**, and
  plain `ctest` has none. `ctest` reports green either way. Run a GPU test
  directly under `vkrun` to know it actually executed.

## Driving a running scene

`--authoring-endpoint <path.sock>` opens a local endpoint; `rxcall <sock>
World.GetPosition <id>` and friends dispatch into the script command registry
(`rx --dump-commands` lists them). It is off unless the flag is passed, the
socket is 0600, and the peer uid must match. Unix socket paths cap at 107 bytes.

Mutations do not yet persist back to the `.rxscene`.
