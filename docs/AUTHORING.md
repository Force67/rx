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
rx --scene scene.rxscene --headless --shot side.png --shot-frames 20 \
   --camera-at "52 34 44" --camera-look "0 10 -24"   # the same scene, second angle
rxdiff before.png after.png           # did it change more than the renderer's own noise
```

Author, validate, render, **look at the png**, fix. `--dump-schema` and
`--dump-materials` are generated, never hand-maintained, so they cannot go
stale. Read them rather than guessing prop names.

`--validate` answers "is this scene correct". It cannot answer "is this scene
any good", and the two failure sets barely overlap: every ugly scene this repo
has shipped validated clean. Looking at the png is not the optional last step,
it is the only step that checks the thing you were actually making.

**Look from a second angle before you believe the first one.** `--camera-at`,
`--camera-look` and `--camera-fov` override the scene's own Camera for one run,
so this costs a flag rather than an edit and an edit back. One hero framing
hides exactly the things worth finding: a facade that only works head-on, a
silhouette that collapses from the side, buildings floating above the ground,
roofs that are flat lids with the wall pattern wrapped over them, a middle
distance with nothing in it.

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
- **`Anchor.target` / `Anchor.mode` / `Anchor.offset`** places an entity against
  another's built geometry (`on | under | left | right | front | behind`), so no
  height is ever written down. It measures the *rotated, stretched* bounds, so a
  turned object sits on its corner correctly. `offset` is where on that face, in
  world axes: standing things on a ground plane, it is the x and z, and the y
  stays derived. **This is how anything sits on anything.** A scene that writes
  its own y values is one prefab edit away from every object floating.
- **`Grid`** lays declared members out on a lattice. `Grid.cell` gives every
  member that does not name its own prefab one to instance.
- **`Rotation.euler`** is degrees, yaw then pitch then roll about the entity's
  own axes.
- **`Stretch.scale`** is a per-axis scale baked into the built `Shape`, on top of
  `Shape.size`. It is a component of its own precisely so it composes with a
  prefab: one building prefab instanced at three proportions is three
  `Stretch.scale` lines, no restating of `Shape.kind` or `Shape.size`. It reaches
  **every part of a multi-entity prefab**, scaling both each part's offset and
  its geometry, so a podium/shaft/crown building is as many silhouettes as it has
  instances. Every axis has to be positive.
- **`Sun`** is the key light as an angle: `elevation` degrees above the horizon,
  `azimuth` degrees about y from +z (the same convention as `Rotation`'s yaw),
  plus colour, intensity and a flat `ambient`. A scene declaring one takes the
  sun over from the day/night clock entirely, which is what makes a capture
  reproducible. See below for why this is the first thing to author, not the
  last.
- **`Atmosphere`** is the air and the film: `density` of haze per metre,
  `start_distance` of clear air before it ramps in, and an `exposure` multiplier
  over the auto-exposure result.

## Putting a real texture on a primitive

`Surface` takes six image maps: `base_color_map`, `normal_map`,
`roughness_map`, `metallic_map`, `occlusion_map`, `emissive_map`. Each is a
path, and each **layers on** the constant beside it exactly as the equivalent
glTF texture does, so a set comes through verbatim only at `base_color = 1 1 1`,
and an `emissive_map` is invisible while `emissive` is `0 0 0`.

```
Surface.base_color = 1 1 1
Surface.base_color_map = "assets/concrete/Concrete034_Color.jpg"
Surface.normal_map     = "assets/concrete/Concrete034_NormalGL.jpg"
Surface.roughness_map  = "assets/concrete/Concrete034_Roughness.jpg"
```

That is the whole of what a CC0 photogrammetry set (ambientCG and friends) needs
to reach an authored primitive. Things worth knowing:

- **Paths resolve relative to the working directory**, like `Model.path` and
  `Surface.materialx` and unlike `Prefab.path`. A path that resolves to no
  image **fails the load** naming the assignment; nothing here substitutes a
  default texture, because a render that quietly used one looks authored.
- **A `Pattern` and a texture map on one entity are refused.** Both bind the
  base colour, normal and roughness of the entity's one material, so either
  would silently overwrite the other. Author one or the other.
- **Take the OpenGL normal map.** A set shipping `_NormalGL` and `_NormalDX`
  means the green channel is flipped between them; the DX one lights inverted
  along v and nothing in the loader can tell them apart.
- `roughness_map` and `metallic_map` are separate greyscale maps, which is how
  a texture set ships them, not glTF's packed ORM. Naming a roughness map also
  stops the shader reading metallic out of its blue channel, so `Surface.metallic`
  stays in charge when there is no metallic map.
- Two surfaces naming the same file share one decode and one upload; two naming
  different files are two materials. Texel density is whatever the primitive's
  uv says (0..1 once per face), the same rule `Pattern.scale` lives under, so a
  big box wants a set that tiles or a `Model` with authored uvs.
- A map named beside a `Surface.materialx` replaces that one slot of the
  document, the same precedence a `Pattern` already has over one.

`Surface.materialx` is the other way in, and takes the whole material from a
`.mtlx` document: both `standard_surface` and `open_pbr_surface` (which is what
ambientCG's exporter writes), and both constant inputs and inputs **connected to
`<image>` / `<tiledimage>` nodes**, optionally through a `<normalmap>`. That is
what makes a downloaded set usable as it ships, since those documents carry no
constants at all. Filenames inside a `.mtlx` resolve against **the document's own
directory**, not the working directory, so a set stays movable as a folder. A
connection through a node this build does not evaluate leaves that slot empty
and says so by name in the log; it never binds an operand as if it were the
result.

Anchors and grids both **replace** `Transform.position`, so an entity cannot use
both, and grid members all share the grid's y. A row of identical lamps is a
grid; a terrace of differing buildings is individual anchors.

## Making it look like something

A scene that validates and still looks bad is the normal case, and it is almost
never the renderer. In rough order of how much each one changes the picture:

**Author the light first.** Without a `Sun` the scene gets whatever hour the
clock was at, which is the single biggest thing deciding what a render looks
like being left to chance. Two numbers do most of the work:

- `elevation` decides how much of the scene is in shadow at all. A low sun
  (5-20) rakes across surfaces, casts long shadows and separates faces that
  point different ways; overhead (60+) flattens everything and hides the
  massing you just built. When a render reads flat, move this before anything.
- `azimuth` decides **which faces you can see are lit**. Light travels *away*
  from the sun, so a sun on the far side of the subject backlights every face
  the camera can see and the whole scene comes back dim and blue. Put it behind
  the camera's shoulder (within ~60 degrees of the view direction, reversed) to
  light what you are looking at, then push it sideways until one side of the
  street is lit and the other is in shade.

Do not fix a dark render by raising `Sun.ambient` or `Atmosphere.exposure`. Both
lift the whole image evenly, which takes the modelling back out of it. Raise
the sun's `intensity`, or move its `azimuth` so the light is landing on
something the camera can see.

**Give the picture depth.** `Atmosphere.density` around 0.008-0.02 separates a
near building from a far one; without it everything renders at the same
contrast at every distance, which is most of why a blockout reads as a diagram.
Past about 0.05 the froxel volume bands at grazing angles on a large ground
plane.

**Give things a silhouette.** One box with a window pattern on it is a box with
a window pattern on it, whatever colour it is. Build a prefab from two or three
masses - a wider podium, a shaft, a set-back crown - and the outline does the
work at any distance. A setback or a 40cm cornice is the difference between a
building and an extrusion. `Stretch` reaches every part, so one such prefab is
as many buildings as you instance it.

**Differentiate the ground floor.** Whatever a viewer is close to is what tells
them how big everything else is. A facade grid that runs unbroken from the
pavement to the roof reads as wallpaper; a taller-glazed base under it reads as
a street.

**Close the frame.** If the composition converges on empty sky, that patch is
the brightest thing in the render and the one place with nothing to look at.
Put something at the end of the view.

**Check the far field and the near field separately.** The far field wants
haze, overlapping silhouettes and nothing small. The near field wants the
clutter: things at hand scale, off the grid, with their own shadows.

## Failure modes worth knowing

- A malformed number, an unknown component or prop, an unknown `Shape.kind`, a
  zero quaternion and a dangling `Anchor.target` all **fail the load** with a
  `path:line:` message. They are never silently substituted.
- `--validate` reports every finding in one pass, including things a strict load
  cannot see (non-finite literals, degenerate scale, duplicate Guids, a
  `Renderable` with no `Transform`, anchor cycles).
- A numeric assignment takes **exactly as many numbers as the prop has**. A
  short list still pads with zeros (a plane is `Shape.size = 9 0`), but a long
  one fails the load: the surplus used to be dropped in silence, which is how
  every city prefab came to carry a two-number `Pattern.scale` against a scalar
  prop and render facades nobody authored, with `--validate` reporting the file
  clean.
- `Pattern.scale` is **two numbers, u then v**, and in **uv space, not world
  space**: a facade is bays across by floors up. Every primitive's uv covers
  0..1 once per face, so the same pattern on a bigger box gives bigger cells.
  Retune per prefab rather than expecting a fixed texel density. One number is
  refused rather than padded, since a zero axis renders as a wall of stripes and
  does not look like a mistake.
- **`Pattern.relief` is in uv units, so it has to shrink as `scale` grows.** The
  generated normal map's slope goes as `relief * scale`; past roughly
  `1/(4*scale)` the normals exceed 45 degrees and the surface breaks into dark
  blotches. A road at `scale = 10 140` wants `relief` around 0.001, not the
  0.006 that looks reasonable next to a `scale = 4` crate. This one always reads
  as a renderer bug and never is.
- A `Stretch` scales a facade's **window cells along with the building**, since
  the uvs are untouched. Beyond about 1.25 the windows read as a different
  building rather than a taller one; get large height differences from different
  prefabs, not from large stretches.
- An `Anchor` centres the target's **whole subtree**, prefab parts included, so a
  prefab with something sticking out one side (an awning, an off-centre roof
  plant) places a little off from where its main mass suggests.
- A glTF's `KHR_texture_transform` (which Blender's exporter emits whenever a
  Mapping node scales the uvs) is **baked into the imported mesh's uvs**, so
  authored texel density survives the import. The extension is per texture
  reference and a uv is per vertex, so a material whose maps ask for different
  transforms keeps its **base colour's** and the loader warns naming the map it
  dropped; the same warning covers a transform pointing at a second uv set,
  which this importer does not read. Nothing about it is silent.
- `Prefab.path` resolves **relative to the file naming it**; `Model.path`,
  `Surface.materialx` and the `Surface` texture maps resolve relative to the
  **working directory**.

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
over a measured 0.00055 floor, which catches a 12% albedo change.

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
