# OpenUSD

rx reads OpenUSD stages (`.usd`, `.usda`, `.usdc`, `.usdz`) into the same
`asset::ImportedScene` the glTF loader fills, so a USD stage and a `.glb` land
in the viewer, the editor and the renderer through one path.

- Viewer: `./build/linux/runtime/rx --usd assets/usd/attic/Attic_NVIDIA.usd`
- Editor: open a `.usd*` file like any other document (File > Open, or drop it in)
- API: `asset::LoadUsdScene(path, &scene)` in `engine/asset/usd_loader.h`

## Building

The reader is [TinyUSDZ](https://github.com/lighttransport/tinyusdz), a
dependency-free C++ USD implementation, vendored the way Jolt and NRD are:

```sh
tools/get_tinyusdz.sh      # clones the pinned tag into third_party/tinyusdz
cmake --preset linux       # RX_USD defaults ON, and off when the checkout is absent
```

`RX_USD=OFF` drops the dependency; `LoadUsdScene` then returns false and says so.
`IsUsdPath` still works, so callers need no `#ifdef`.

## Getting the NVIDIA sample scenes

`tools/get_usd_samples.sh` pulls from NVIDIA's public Omniverse content bucket:

| argument    | scene                          | size    |
| ----------- | ------------------------------ | ------- |
| `templates` | small studio/interior stages    | ~70 MB  |
| `attic`     | `Samples/OldAttic` (Attic_NVIDIA) | ~1.6 GB |
| `astronaut` | `Samples/Astronaut`             | ~530 MB |
| `marbles`   | `Samples/Marbles`               | ~1.1 GB |

```sh
tools/get_usd_samples.sh attic
./build/linux/runtime/rx --usd assets/usd/attic/Attic_NVIDIA.usd
```

They land in `assets/`, which is gitignored.

## What the importer does

**Composition runs before anything else.** Opening a layer only parses it, so
`LoadUsdScene` drives LIVRPS itself: subLayers first, then references, payloads,
inherits, variant selection and specializes, iterated to a fixed point because
resolving one arc can introduce more. Variant selection is deferred while
references or payloads are still outstanding (AOUSD 10.3.2.5), and referenced
layers are parsed once and shared across every arc that names them.

**Stage conventions are normalized away.** A `upAxis = "Z"` stage is rotated into
the engine's y-up, and `metersPerUnit` is folded into the instance transforms, so
a centimetre z-up Omniverse scene and a metre y-up glTF arrive in the same world
space.

**Geometry** is triangulated and made single-indexable (one index buffer for
positions, normals, uvs and tangents, which is what the engine's vertex format
wants). `materialBind` GeomSubsets become submeshes; faces no subset claims get a
trailing submesh on the mesh-wide binding. `orientation = "leftHanded"` flips the
winding. Normals, uv set 0, tangents/binormals and displayColor/displayOpacity
come across; v is flipped into the engine's convention.

**Materials** map UsdPreviewSurface onto the engine's metallic-roughness model:
diffuseColor, opacity, metallic, roughness, normal, occlusion, emissiveColor,
ior, clearcoat, and the opacityThreshold/opacity-derived alpha mode. Metallic and
roughness are separate inputs in USD, so when they resolve to one image the
combined glTF-style path is used and otherwise metallic gets its own slot.

**Textures** are decoded to rgba8 through rx's own loader rather than tinyusdz's,
which resolves paths relative to the stage; single-channel maps replicate into
rgb so a greyscale roughness map still reads through `.g`.

## Limitations

- **MDL materials render as untextured defaults.** NVIDIA's Omniverse content is
  largely authored in MDL (`outputs:mdl:surface`), which needs NVIDIA's MDL SDK
  to evaluate - no USD reader, including Pixar's, shades these on its own. Only
  the prims that also carry a UsdPreviewSurface get their real look. The importer
  logs how many materials had no UsdPreviewSurface.
- **Mirrored instances lose their mirroring.** `ImportedScene::Instance` is a
  position, a quaternion and one uniform scale, which cannot express a negative
  determinant. The count is logged.
- **No skinning, blend shapes or animation yet.** UsdSkel and time-sampled
  transforms are read by tinyusdz but not yet mapped onto rx's skeletons; USD
  stages import as static geometry. glTF remains the path for animated content.
- **usdz packages are not composed.** A package's root layer is read through
  tinyusdz's package reader (which is what picks the root layer correctly), but
  arcs between layers inside the zip are not resolved. Packages are self-contained
  and usually pre-flattened, so this rarely matters.
- **Lights and cameras are dropped.** tydra converts them; rx does not consume
  them yet, so scene lighting comes from the engine's own setup.
- rx does not write USD. This is an import path only.

## Two upstream quirks worth knowing

Both are handled in `usd_loader.cc`, and both cost whole scenes if they are not:

- Omniverse writes `colorSpace = "RAW"`; tinyusdz matches `"raw"`/`"Raw"` and
  treats an unknown token as a hard error that fails the entire stage, not just
  that material. The importer lowercases colorSpace tokens on the composed layer
  first - every token tinyusdz accepts survives that.
- tinyusdz rejects asset paths beginning with `..` (a sandbox escape for a
  resolver serving remote assets). Authored scenes use them constantly for
  sibling texture and prop directories, so composition is configured to allow
  them and texture loading goes through rx's own resolver.

## Lighting: a stage brings its own rig

A scene lit by the viewer's procedural sun instead of the rig it shipped with
does not resemble its source, so the importer takes lights, cameras and the
authoring tool's render settings over from the stage.

- **DistantLight** becomes the sun, **DomeLight** becomes the sky (its HDR is
  installed as the environment map, so IBL comes from the scene's own sky rather
  than the procedural atmosphere), and sphere/rect/disk/cylinder lights become
  the engine's punctual area lights.
- **The authored camera** supplies position, orientation and vertical fov. A
  stage camera is a lens as much as a pose: the Attic's is 18 mm, which frames
  nothing like the engine's default 60 degrees.
- **Inherited `visibility` is honoured.** This matters more than it sounds -
  see below.

UsdLux intensity is photometric and its absolute scale is a per-DCC convention
rather than anything the spec pins down (Omniverse writes a daylight sun at
15000 and a practical bulb in the millions, against an engine range where a
campfire is 9). The mapping is therefore tunable rather than hard-coded:

    RX_USD_SUN_SCALE      sun            (default 2.7e-4)
    RX_USD_DOME_SCALE     dome / ibl     (default 1e-5)
    RX_USD_LIGHT_SCALE    punctual       (default 4e-3)
    RX_USD_LIGHT_MAX      per-light cap  (default 40)
    RX_USD_DOME_ROTATION  dome yaw, radians
    RX_USD_CAMERA=0       ignore the authored camera
    RX_USD_INTERIOR=0     keep the procedural sky (an exterior stage wants it)
    RX_USD_CAMERAFX=1     restore lens flare and bloom (off for imported scenes,
                          because both halo hard in a dark interior with bright
                          windows)

### Picking between rigs a stage ships

Scenes routinely carry several mutually exclusive configurations in one file and
switch between them with `visibility`. The Attic ships a **day** rig and a
**night** rig plus a 500-bulb string-light strand that is switched off; its
`subLayers` are `[Night, Day, ...]`, and USD resolves the earlier sublayer as
the stronger one, so **night is the file's authored default** and that is what
rx renders with no flags.

NVIDIA's marketing images of this scene are the *day* rig - the warm golden one.
To get it, override visibility:

    rx --usd assets/usd/attic/Attic_NVIDIA.usd \
       --usd-show /Root/Lights --usd-hide /Root/LightsNight

`--usd-show` / `--usd-hide` take absolute prim paths, cover their subtrees, are
repeatable, and `--usd-hide` wins over `--usd-show`. They override authored
visibility only; nothing in the stage is modified.

## Render settings

USD has no standard schema for renderer configuration - UsdRender describes
outputs, not light transport, and the Attic authors no UsdRender prims at all.
Omniverse instead records its renderer's state as an `rtx:` dictionary in the
layer's `customLayerData`, and rx reads the parts that map onto engine features.
It is vendor metadata rather than something portable, so every field is
advisory: unauthored settings leave the engine default alone.

| authored | used for |
| --- | --- |
| `rtx:indirectDiffuse:scalingFactor` | indirect diffuse (GI) intensity |
| `rtx:sceneDb:ambientLight*` | flat ambient, only when no dome drives IBL |
| `rtx:fog:*` | froxel fog density and start distance |
| `rtx:post:lensFlares:*` | lens flare scale, when camera effects are enabled |

The indirect scale is the one that changes the picture. A path-traced interior
is mostly bounce light - the Attic asks for **7x** - and at 1x the same geometry
reads as an unlit box with a bright window in it.

Fog is authored as distance fog with the near field kept clear (the Attic starts
at 10 m, about the width of the room), which is what lets shafts read as beams
across the space instead of haze sitting on the lens. `RX_FROXEL_DENSITY` and
`RX_FROXEL_START` override the authored values independently.
