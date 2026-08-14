# Decals

rx has two decal paths, and they are not competing implementations of the same
thing — they answer different questions.

| | Clustered projectors | Baked texture-space layers |
| --- | --- | --- |
| Where it lives | `render/pipeline/mesh_pipeline.h` (`Decal`) | `render/texturing/decal_bake.h` (`DecalBaker`) |
| Cost per pixel | grows with the decals in the froxel | one fetch, always |
| Budget | 128 per frame, 16 per cluster | unlimited per receiver |
| Applies to | any surface the box overlaps | one receiver's own UV space |
| Lifetime | per frame; the app resubmits | permanent until washed off |
| Needs | nothing | unique (non-overlapping) UV0 |
| Good for | bullet holes on level geometry, puddles, glowing runes, anything short-lived or shared | fluid splatter and tattoos on characters, damage that accumulates |

The projector path is the classic one: a box is clustered with the lights and
every pixel inside it re-evaluates the decal while shading. That is exactly
right for a handful of decals on static geometry, and exactly wrong for a
character that keeps taking hits — the tenth splat costs the same as the first,
every frame, forever.

The rest of this document is about the second path.

## The idea

A projected decal is rasterized **once**, into a small per-instance layer that
lives in the receiver's own UV space. The forward pass then composites that
layer over the material with a single extra texture fetch, so a character
carrying two hundred blood splats shades exactly as fast as a clean one.

```
 stamp (world-space box)                       forward pass
        │                                            │
        ▼                                            ▼
 rasterize the receiver's mesh          albedo = over(albedo, layer)
 INTO ITS UV SPACE, test each       ◄── n, roughness perturbed by fx
 texel's world pos against the box
        │                                     ▲
        ▼                                     │
   albedo / fx / chart tile ──── dilate ── mips
```

The naive alternative is to clone the character's base-colour texture per
instance and paint into it. A 2048² BC7 map is 5.6 MB compressed but 16 MB
uncompressed, per actor. A layer tile is 256² × 2 = 0.5 MB, and it is the only
thing that has to be per-instance: the material's own maps stay shared.

## Memory

Three shared atlases, sized at `DecalBaker::Initialize`:

| Atlas | Format | Holds |
| --- | --- | --- |
| albedo | RGBA8 + mips | premultiplied decal colour, `a` = coverage |
| fx | RGBA8 + mips | tangent-space normal `xy`, roughness multiplier, coverage |
| chart | R8 | 1 where the receiver's UV charts are |

The default (`atlas_size` 1024, `tile_size` 256) is 16 receivers for ~11 MB.
A character-heavy game wants 2048/512: 16 receivers at 512² for ~47 MB.
Tiles are allocated **lazily** — a receiver that has never been hit costs
nothing but a handle.

### Rebaking, which is the point

Tiles are recycled LRU, and a receiver whose tile was taken while it was off
screen does not lose its decals. Every stamp is journalled CPU-side (~112
bytes), and the next time the receiver draws, its whole history replays in **one
draw** into a fresh tile. That is the trade the system is built around: keep the
cheap description, throw away the expensive pixels, and pay a single bake to get
them back.

The journal is capped (`journal_limit`, 48 by default); older stamps fall off
the front, which is invisible in practice because later splats cover them.

## Using it

```cpp
// once per actor
actor.decal_receiver = renderer.AcquireDecalReceiver();

// every frame
render::DrawItem draw;
draw.mesh = actor.mesh;
draw.transform = actor.transform;
draw.skin_offset = actor.skin_offset;
draw.decal_receiver = actor.decal_receiver;
view.draws.push_back(draw);

// when something hits
render::DecalStamp splat;
splat.receiver = actor.decal_receiver;
splat.projector = render::MakeDecalProjector(hit_point, hit_normal, up,
                                             /*width*/ 0.32f, /*height*/ 0.32f,
                                             /*depth*/ 0.7f);
splat.projector.uv_rect[0] = 0.5f;   // atlas page: scale.xy, offset.zw
splat.projector.tint_blend[3] = 0.95f;  // opacity
splat.projector.params2[0] = 0.9f;   // 3d fx: normal perturbation strength
splat.projector.params2[1] = 0.25f;  // 3d fx: roughness multiplier (wet gloss)
view.decal_stamps.push_back(splat);  // or renderer.StampDecal(splat)
// Both are equivalent: RenderFrame queues the list before it can bail out on a
// skipped frame, since a stamp is a one-shot event the app will not resubmit.

renderer.ClearDecals(actor.decal_receiver);    // wash it off
renderer.ReleaseDecalReceiver(actor.decal_receiver);  // actor died
```

The projector is the same oriented box the clustered path uses and it samples
the same authored atlas (`Renderer::SetDecalAtlas`), so a game can throw one
decal either way without re-authoring it. With no atlas bound the baker
substitutes a built-in white page, and a stamp paints its own footprint in flat
colour — useful on its own, and it keeps the pass valid in a scene that never
loaded decal art.

ECS apps skip the manual `DrawItem` field: put a `scene::DecalReceiver` on the
entity and `Host::GatherEntityDraws` carries it, exactly like `scene::Tint`.

**2D vs 3D fx.** `params2[0] = 0` gives a flat decal that only replaces albedo —
a tattoo, a painted marking, a logo. Non-zero adds the source normal map (rotated
out of the projector's basis into the receiver's tangent frame) and a roughness
multiplier, so a fluid splat raises a rim and catches a specular highlight like
real wet fluid. Emissive decals stay on the projector path: the layer's four
channels per atlas are fully spoken for, and a handful of glowing runes is what
the clustered loop is good at.

## UDIM and other uv layouts

A receiver's uvs do not have to be a 0..1 square. `SetDecalReceiverUv` applies
`layer = uv * scale + bias` before anything else, and geometry that lands
outside 0..1 after that transform takes no decal at all — in the bake (the
scissor drops it) and in the forward pass (an explicit reject), so the two can
never disagree.

That is what makes imported characters work. A Daz/Genesis figure is UDIM: the
body's zones are laid out across `u` in `[0, 7)`, one integer tile each. Biasing
a receiver by `-2` addresses the torso zone and hands it the whole layer at full
resolution; the arms, legs and face keep their skin because their uvs fall
outside the mapped square. Without this the `frac()` fallback would wrap every
zone onto the same tile and one chest tattoo would print on all seven.

Sizing follows from that: the layer tile has to hold a whole uv zone, so
tattoo-grade detail wants a bigger tile than splatter does. `RX_DECAL_ATLAS`
and `RX_DECAL_TILE` override `Desc` without a rebuild — 2048/1024 gives a
Genesis torso zone a full 1024² to itself.

## Requirements and limits

- **Unique UV0.** Charts must not overlap. Character and prop UVs normally
  qualify; tiling architecture UVs do not, and a decal stamped on one would
  repeat across every tile. `asset::MakeSkinnedBiped` packs its boxes into a
  UV atlas for exactly this reason.
- **One zone per draw.** The tile index is per-draw, so a receiver addresses one
  uv zone. A character wanting decals on torso AND arms needs a receiver per
  zone, which today means a draw per zone.
- **One tile per mesh.** Submeshes that each re-use the full 0..1 square share a
  tile, and a decal on one shows on the others.
- **Raster and mesh-shader paths.** The tile index rides the top byte of the
  push-constant tint word. The mesh-shader path shares the FRAGMENT stage, and
  therefore the push range, with the raster path, so `MeshShaderPush` mirrors
  the first 144 bytes of `MeshPushConstants` and leaves the tile at 0: meshlet
  props take no layer. A `static_assert` ties the two offsets together, because
  anything else landing on that word is read as a tile (`bounds.w` used to, and
  the pixel shader decoded a bounding radius as tile 63).
- **Skinning is posed, morphs are not.** The bake poses vertices through the
  same bone palette as the scene vertex shader, so a splat lands where it hit
  and then rides the animation. Morph-target deltas are not applied at bake
  time; a decal on a heavily morphed face bakes against the un-morphed surface.

## How the bake pass works

`DecalBaker::AddToGraph` records nothing at all in a frame where nothing
changed. When it does have work:

1. **Clear.** A tile taking new ownership gets its neutral content copied in
   from a staging buffer filled once at startup (transparent albedo, flat
   normal and unity roughness in fx, empty chart). This is also what wipes the
   previous owner.
2. **Stamp.** One draw per receiver, scissored to its tile, rasterizing the
   mesh with `sv_position = uv * 2 - 1`. The pixel shader loops over that
   receiver's whole stamp run and emits ONE premultiplied composite, which the
   fixed-function blend puts over the tile — so a 48-stamp journal replay is a
   single draw, not 48. The third target writes the chart mask, free, because
   the draw already covers the whole mesh.
3. **Dilate.** A UV chart's border texels are only partially covered by the
   rasterizer, so bilinear filtering in the forward pass would reach texels the
   bake never wrote and a splat crossing a seam would show a hairline crack.
   The dilate copies the best in-chart neighbour into every out-of-chart texel
   that touches the chart. It only writes texels the mask calls outside and only
   reads ones it calls inside, so it is idempotent — repeated bakes cannot make
   the gutter creep outward.
4. **Mips.** A blit chain over the whole atlas. A mip texel never spans two
   tiles (the chain stops far short of a tile edge), so the downsample cannot
   bleed between them. The forward pass additionally clamps its sample to a
   guard band of half a texel at the coarsest mip, because a filter TAP reaches
   past the tile edge even when the texels themselves do not.

## Testing

`test/decal_bake_test.cc` bakes a stamp onto a quad and reads the atlas back,
then evicts the tile out from under the receiver and asserts the journal replays
it. Run it on a real GPU, with validation:

```
vkrun ./build/linux/decal_bake_test
RX_VALIDATION=1 vkrun ./build/linux/decal_bake_test
```

It also covers the UDIM case: a quad whose uvs sit on tile 2 bakes nothing until
the receiver is biased onto that tile, then bakes normally.

The feature gym's animation district throws splatter at the walking actor every
0.8 s and stamps a flat tattoo on its chest at spawn:

```
DISPLAY=:10 vkrun ./build/linux/runtime/rx --demo featuregym
```

### On an imported character

`RX_TATTOO="fx,fy,fz,size[;...]"` bakes tattoos onto the heaviest mesh of a
`--gltf` scene. Anchors are fractions of that mesh's local bounds; each snaps to
the nearest vertex, so the projector sits on the skin along its normal, and that
vertex's uv picks the UDIM tile the receiver is biased onto.
`RX_TATTOO_ATLAS=<file>` supplies the ink as a raw square RGBA8 blob.

```
blender --background character.blend --python tools/blend_to_glb.py -- \
    --output character.glb --manifest character.manifest

RX_DECAL_ATLAS=2048 RX_DECAL_TILE=1024 RX_TATTOO_ATLAS=ink.rgba \
  RX_TATTOO="0.50,0.755,0.95,0.16;0.435,0.63,0.95,0.10" \
  vkrun ./build/linux/runtime/rx --gltf character.glb
```

Note that `tools/blend_to_glb.py` exports skins, and the viewer's `--gltf` path
supplies no bone palette for them: the skinned vertex shader then reads a null
palette address and the GPU stops presenting. Export the body with
`export_skins=False` (bind pose) until that is fixed.
