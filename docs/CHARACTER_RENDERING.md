# Character rendering

Skin, eyes, teeth and the bench that decides whether any of it is right.

The design follows the transferable half of The Callisto Protocol's character
work. The lesson there is not a shader equation:

> Calibrated photography is the ground truth. The renderer is an approximation,
> adjusted until it matches reality.

Everything below is organised around being able to *do* that — measure, adjust,
measure again — rather than around any one lobe.

```
reference / colour pipeline
   -> human material parameters
      -> direct BRDF (one evaluator, every light type)
         -> skin transport
            -> eye / mouth specialist shading
               -> layered appearance + dual normals
                  -> lighting, shadows, reflections
                     -> optional measured residual
```

## Quick start

```sh
tools/get_head_scan.sh          # a photogrammetry human, 8K albedo + normal
cmake --build build/linux
build/linux/runtime/rx --demo lookdev
```

The bench opens on the frozen rig. Arrow keys cycle OLAT lights (left/right) and
camera stops (up/down); `C` cycles comparison modes; `Z`/`X` undo/redo; `R`
resets to the region presets.

| Piece | Where |
| --- | --- |
| The BRDF | `engine/render/shaders/human_brdf.hlsli` |
| The eye | `engine/render/shaders/human_eye.hlsli` |
| CPU side, presets, tiers, CPU mirror | `engine/render/pipeline/human_material.{h,cc}` |
| Authored parameters | `asset::Material::HumanParams` (`engine/asset/material.h`) |
| Reference comparison pass | `engine/render/post/reference_compare.{h,cc}` |
| The bench | `runtime/demo_lookdev.{h,cc}` (`--demo lookdev`) |
| Residual fitting | `tools/fit_residual.py` |
| Regression test | `test/human_brdf_test.cc` |

## 1. The one contract that matters

**The neutral parameter set reproduces the engine's stock Lambert + GGX
exactly.** Turning the character model on changes nothing until somebody dials a
knob against reference.

This is not a nicety. Without it, "enable the character model" silently
re-shades every material that opted in, and every fit made before the change is
worthless after it. It is defended in two places:

- `test/human_brdf_test.cc` diffs the CPU mirror against the stock BRDF over a
  sweep of roughness, view and light directions — *including a non-zero light
  solid angle*, which is what caught `light_shape_response` re-shading every
  highlight in the frame when it was on by default.
- End to end, in the real renderer, on the real asset:

  ```sh
  RX_LOOKDEV_NEUTRAL=1 RX_UI_SHOT=/tmp/a.png RX_UI_SHOT_FRAMES=90 \
    build/linux/runtime/rx --demo lookdev
  RX_LOOKDEV_HUMAN=0   RX_UI_SHOT=/tmp/b.png RX_UI_SHOT_FRAMES=90 \
    build/linux/runtime/rx --demo lookdev
  # /tmp/a.png and /tmp/b.png must differ by at most 8-bit rounding.
  ```

## 2. One evaluator, every light

The second contract: **every light type that touches a character evaluates the
same material model.** A face that shades differently under the sun than under a
spot is not a material, it is two materials that happen to share a texture.

`HumanEvaluate` in `human_brdf.hlsli` is the only place the character response is
computed. It is called from:

| Path | Lights routed through it |
| --- | --- |
| `mesh.ps.hlsl` (raster) | sun, point, spot, sphere (LTC), rect panel (LTC), ReSTIR DI |
| `mesh_rt.ps.hlsl` (hybrid RT) | the same set, with traced sun shadows |
| `gi/pathtrace.cs.hlsl` (reference tracer) | sun NEE |

Area lights whose *shape* the caller already integrated (the LTC panels and
spheres) go through `HumanEvaluatePreintegrated`, which applies the material's
directional shaping once at the representative direction. That is an
approximation, and it is documented as one at the call site — the alternative,
letting the panel path grow its own material model, is the failure this design
exists to prevent.

The ray paths read their parameters from `BindlessRegistry::MaterialRecord`, and
`MaterialSystem::UpdateMaterialParams` refreshes the record *and* the uniform
together, so a look-dev slider cannot move the rastered face while the traced
face stays put.

## 3. The controls

Every control is independent, monotonic in its own direction, and neutral at its
default. The forms are the engine's own fits — the published Callisto slides give
indicative shapes, not production constants. **Fit them against your own
reference. Do not copy numbers out of a paper.**

### Diffuse

| Control | What it does | Safe range |
| --- | --- | --- |
| `diffuse_fresnel_peak` | Grazing gain on the diffuse lobe — the boundary transmission loss, entered and left. Negative darkens. | −0.5 … 1.0 |
| `diffuse_fresnel_falloff` | Shapes the **view** half of that. | 1 … 8 |
| `diffuse_fresnel_tangent_falloff` | Shapes the **light** half. Equal values keep the lobe reciprocal. | 1 … 8 |
| `retroreflection_peak` | Back-scatter toward the light: the velvety lift skin shows with the key behind the camera. Burley's shape, artist-keyed. | 0 … 2 |
| `retroreflection_falloff` / `_tangent_falloff` | Its light / view halves. | 1 … 8 |
| `smooth_terminator_amount` | How much of the terminator is softened. | 0 … 1 |
| `smooth_terminator_length` | How far past the geometric terminator light wraps, in cosine units. | 0 … 0.5 |

The terminator softening is **energy normalized** (Hill's wrapped cosine): it
moves light, it does not create it, and the test asserts the hemispherical
integral does not rise. It is also gated by the *geometric* normal — a normal map
may soften a terminator, never carry light around the far side of a head. And it
applies to the diffuse lobe only: a soft terminator is a subsurface transport
effect, and letting it widen the highlight is exactly the "looks like wax"
failure.

### Specular

| Control | What it does | Safe range |
| --- | --- | --- |
| `specular_fresnel_falloff` | Generalized Schlick exponent. 5 = classic. | 2 … 8 |
| `secondary_roughness_scale` | The second GGX lobe's roughness, as a multiple of the first. Below 1 the "tail" is tighter than the core, which is not a tail. | 1 … 8 |
| `secondary_specular_weight` | How much of it. **Blended, not added** — total specular energy is unchanged. | 0 … 1 |
| `light_shape_response` | How much of a light's solid angle the lobe absorbs. 1 = a light cannot produce a highlight tighter than its own image. **0 in the neutral set**, because the stock path treats every light as punctual. | 0 … 1 |

A single GGX cannot hold both a tight core and the broad tail a real dermis and
oil stack throws. The second lobe is the answer, and it is the first thing the
Standard tier drops — the most cost per pixel of visible difference at gameplay
distance.

### Transport

| Control | What it does | Safe range |
| --- | --- | --- |
| `mean_free_path` | Metres. Skin's red channel is ~1 mm. | 0.0001 … 0.01 |
| `subsurface_scale` | Uniform multiplier (thicker / thinner skin). | 0.1 … 4 |
| `transmission` | The through-the-surface lobe: ears, nostrils, eyelids, fingers. | 0 … 1 |
| `transmission_tint` | What survives the crossing. | — |
| `extinction_scale` | Thickness → optical depth. | 0.1 … 8 |
| `thickness_scale` | Metres at `thickness_map` == 1; used directly when no map is bound. | 0.0005 … 0.1 |

The transmission lobe needs the light that entered the **far** side, which the
ordinary shadow test correctly reports as occluded — by the surface's own back
face. Both paths handle it explicitly:

- raster: the cascade comparison reference is pushed `thickness` metres toward
  the light, using the exact world-to-NDC depth scale read off the cascade
  matrix's z row. An occluder inside that distance is the surface itself
  (transmit); anything further is a real blocker.
- RT: the shadow ray starts `thickness` metres *through* the surface.

Screen-space diffusion (`sss_blur.cs.hlsl`) still owns the lateral bleed; the
analytic model owns the surface. Seed both from the same mean free path — the
bench does.

### Layers

| Control | What it does |
| --- | --- |
| `corneal_wetness` | A near-mirror lobe over the base (tear film, saliva, sweat sheen) that dims what is under it by its own reflectance, so it adds no free energy. |
| `cavity_occlusion` | Darkens the **indirect** term only. A mouth interior loses ambient, not the light you shine into it. |
| `specular_normal` + `specular_normal_strength` | **Ns**, the specular shading normal. |

The diffuse/specular normal split is the single highest-value layering feature.
Sweat has to bend the highlight without bending the diffuse; share one normal and
droplets turn skin into scarred geometry the moment the key light moves off axis.
`--demo lookdev` binds a procedural sweat normal on every skin part at strength
0, so the A/B is one slider away and never reallocates a binding set.

Strength 0 is *exactly* Nd, not "a flat map applied" — a flat tangent-space
normal resolves to the geometric normal, which is a different vector wherever a
diffuse normal map is doing anything.

### Eyes

The eye is shaded as a layered anatomical system on one mesh, in the order the
research insists on: geometry, depth, parallax, occlusion, roughness first.
There is deliberately **no spectral thin-film simulation** in this file.

| Control | What it does |
| --- | --- |
| `iris_depth` | Metres behind the corneal surface. The iris is *sampled* there, through a refracted view ray — not decalled onto the surface. |
| `iris_radius` | uv radius of the iris disc. |
| `pupil_scale` | Dilation, as a radial remap that leaves the limbus fixed — dilating cannot slide the iris edge. |
| `limbal_ring_size` / `_power` | The darkened annulus at the iris/sclera boundary. Applied to *albedo*, so it survives every light path identically. |
| `cornea_ior` | Refraction at the corneal surface. |
| `iris_shadow_depth` | The limbus occluding oblique light before it reaches the pigment — evaluated separately from the corneal surface's own shadow. |

The corneal reflection is evaluated on the **unperturbed** corneal normal. That
is what keeps a catchlight from swimming while the eye rotates: only the iris
lookup moves.

`iris_depth` is in metres and the uv density is measured from screen-space
derivatives, so the parallax is correct on any uv layout.

### Regions

`HumanRegion` selects a fitted starting point: `kSkin`, `kLips`, `kTeeth`,
`kGums`, `kSclera`, `kCornea`, `kIris`, `kTearline`. Each preset's rationale is
in `human_material.cc` — teeth, for instance, get a strong diffuse Fresnel
because that is what makes enamel read as glassy rather than as painted bone, and
a saliva film plus cavity occlusion because a tooth lit like an isolated opaque
object never looks like it is in a mouth.

`--demo lookdev` guesses the region from the source material's name and lets you
override it per part.

## 4. Fitting order

Do not tune everything at once. The bench's fitting stages follow this order, and
it is the order for hand-tuning too.

1. **Frontal match** — base colour, exposure, white balance, primary roughness,
   primary specular. Everything after this is fitted against whatever exposure
   you settle here, so settling it late invalidates the rest.
2. **Specular shape** — `secondary_roughness_scale`, `secondary_specular_weight`,
   `specular_fresnel_falloff`, `light_shape_response`.
3. **Retroreflection** — peak and falloff.
4. **Side-light terminator** — amount and length.
5. **Grazing response** — diffuse Fresnel, its tint and falloffs.
6. **Skin transport** — SSS, mean free path, transmission, thickness.
7. **Art-direction layers** — wrinkles, sweat, dirt, blood, damage, wetness.
8. **Residual correction** — only once every stage above is stable.

## 5. The bench (`--demo lookdev`)

### Subject

`RX_LOOKDEV_SUBJECT=<file.glb>`, else `--gltf`, else `assets/head/head.glb`, else
a procedural stand-in. The stand-in is deliberately anatomical rather than a
sphere: the terminator, the transmission and the eye path all need curvature,
thin parts and an actual eyeball to say anything, and a bench that only works
once you have downloaded a 130 MB scan is a bench nobody opens.

The subject is framed from the centroid of its own top slice, so a posed
full-body scan gets its head framed rather than its navel.

### OLAT rig

One light at a time. Two lights at once make a parameter's effect
unattributable, and every fit done that way lands on a compromise nobody chose.
Ambient and IBL are off by default for the same reason — they are a second,
omnidirectional light.

| # | Stop | | # | Stop |
| --- | --- | --- | --- | --- |
| 00 | ambient only | | 07 | grazing 110° |
| 01 | front soft panel (rect) | | 08 | back |
| 02 | front hard point (sphere) | | 09 | top |
| 03 | front sun | | 10 | bottom |
| 04 | key 30° | | 11 | large rect, side |
| 05 | key 45° | | 12 | small point, side |
| 06 | side 90° | | 13 | three lights |

Stops 01/02 and 11/12 are the emitter-shape pairs, and they are the parity test
for "every light type evaluates the same material": same nominal direction, same
**illuminance**, opposite extremes of shape. The stops are authored as an
illuminance target and converted to radiance through each emitter's own solid
angle — an area light's `intensity` is radiance, so a 0.9 m panel and an 8 mm ball
at the same number differ by three orders of magnitude in how much light they put
on a face. Measured over the 14 stops, mean face luminance lands within
0.19 – 0.36 (0.49 for the three-light stop), which is what makes them comparable
at a fixed exposure.

### Camera stops

front, 30°, three-quarter, profile, close-up, gameplay distance, LOD transition
distance. The last two exist because a material optimized only for the close-up
is the classic way to ship a face that falls apart in play.

### The colour and exposure contract

The bench renders **native** (no upscaler), with auto-exposure off and bloom,
lens flare, chromatic aberration, vignette, grain, depth of field and motion blur
all disabled. Every one of those is a difference between the reference and the
render that has nothing to do with the material; a comparison that includes them
measures the lens.

### Reference comparison

`ReferenceCompare` runs on the **scene-linear** image immediately before
exposure and tonemap, so the reference and the render travel the same colour
path. Comparing after the display transform would measure the tonemap.

Modes: side by side, wipe, linear difference, display-referred difference,
reference only. `.hdr` sources load scene-linear; 8-bit sources are de-gamma'd on
the way in. Alignment is a uv scale + offset; outside the aligned rectangle the
render passes through untouched, because a black "difference" where there is no
reference would read as a match.

A four-channel region mask (r skin, g eyes, b lips, a teeth) buckets the error
metric. Fitting one material against a mask that mixes four materials converges
on none of them.

The metric is measured **display-referred**: a linear metric is dominated by the
highlights and will happily trade a wrong terminator for a slightly better
specular core.

### Automated fitting

Coordinate descent over the fit fields, scored on the measured error summed over
**every selected OLAT stop at once**. Three properties make it a measurement
rather than a preference: it only moves fields at or below the current stage; it
cannot buy a win under the key by losing under the rim; and it accepts a step
only if the summed error actually drops.

### Deterministic capture

```sh
RX_FIXED_DT=0.0166667 RX_LOOKDEV_SHOTS=build/lookdev-shots RX_LOOKDEV_QUIT=1 \
  build/linux/runtime/rx --demo lookdev
```

Walks the full matrix — 14 lights × 7 cameras = 98 frames — and exits. The rig is
frozen, so a diff between two runs is a renderer change and nothing else.

### Per-lobe debug views

`RX_DEBUG_VIEW=<n>` or the overlay's Debug view combo:

| n | View |
| --- | --- |
| 16 | character diffuse lobe |
| 17 | character specular lobe |
| 18 | the second GGX lobe's own contribution |
| 19 | the transmission lobe |
| 20 | the measured residual |
| 21 | \|Ns − Nd\|, the normal split |
| 22 | eye: r iris mask, g limbal ring, b iris shadow |
| 23 | uv (feeds `tools/fit_residual.py`) |

These read the **same accumulators the lit path filled**, so they verify the
shipped evaluation rather than a second copy of it.

## 6. Quality tiers

A tier is a *reduction of the same model*, never a different one. A face that
changes material semantics at 8 m is the defect the tiers exist to prevent, and
the test asserts the reduction is monotonic.

| | Hero | Standard | Distant |
| --- | --- | --- | --- |
| BRDF controls | full | full | neutral |
| Second GGX lobe | yes | no | no |
| Separate normals | yes | yes | flattened |
| SSS | full | half-res | simplified |
| Transmission | full | simplified | none |
| Eye refraction | full | simplified | none |
| Residual | optional | no | no |

`HumanTierForScreenHeight` gives the nominal edges (≥360 px hero, ≥64 px
standard). `RenderSettings::human_tier_cap` — set by the quality presets — caps
the whole cast for the hardware; the tier a character actually gets is the lower
of the two. The renderer does not apply it on its own: the tier is baked into a
material's parameters at upload, so the **app** picks per character and calls
`Renderer::UpdateMaterial`.

## 7. The measured residual (optional, last)

```
photograph - analytical render = residual
```

`tools/fit_residual.py` measures it under calibrated OLAT frames, projects it
into texture space through a uv capture (debug view 23), fits it to a directional
basis and writes two maps. The runtime evaluates

```
residual = ambient * max(1 + dot(direction, l_tangent), 0) * coverage * weight * validity
```

Both maps are **signed**, stored biased (`v*0.5+0.5`) — a residual is a
difference and is negative wherever the analytic model is too bright, which is
half of what there is to correct. The alpha of the ambient map is the fit's own
coverage, which is why an all-zero default texture is the neutral one.

The fit is deliberately low order (L0 + L1). A high-order fit of a bad analytic
model just bakes the model's errors into a texture and freezes them. On synthetic
ground truth the fit reconstructs **withheld** light directions to ~2.5% of the
residual's own magnitude.

`validity` is computed at runtime from how far decals, blood, dirt or a wet film
have moved the albedo from what was captured, plus the material's own weight. A
corrected face must not keep a correction that was measured on a clean one.

**Do not start here.** Realis is a final correction layer over a renderer that is
already right. Build the bench and the BRDF first.

## 8. Versioning and budgets

`render::kHumanModelVersion` versions the parameters' **semantics**, not the
implementation. Bump it when a control changes meaning, range or default. Look-dev
presets carry it and refuse to load under a different one: fitted numbers are
measurements against a specific model, and a preset silently reinterpreted under
a newer one still looks plausible, which is harder to catch than a refusal.

The bench carries a GPU frame-time budget with an alarm. The hero tier is the one
that quietly grows past its budget, because everything that makes it hero-tier is
invisible in a still and obvious in a frame time.

Texture memory is the engine's existing `texture_budget_mb` streaming budget; an
8K albedo plus an 8K normal per hero character is the number to watch.

## 9. Validation matrix

Test every significant change across:

- **Views** — front, 30°, three-quarter, profile, close-up, gameplay distance,
  LOD transition.
- **Lights** — frontal soft, frontal hard, side, grazing, back, top, bottom,
  large rect emitter, small point emitter, multiple lights, dark scene at high
  exposure.
- **States** — neutral, eye rotation, blink, mouth open, teeth visible,
  wrinkles, sweat, dirt, blood, mixed layers.
- **Motion** — camera orbit, character turn, light orbit, eye movement, LOD
  transition, resolution change, temporal reconstruction reset, exposure
  adaptation.

The bench's capture pass covers views × lights automatically. States and motion
are driven from the panel.

## 10. Things not to do

- Do not start with the residual before the base renderer is stable.
- Do not copy shader constants recovered from slides into production.
- Do not fit a material against a single hero image.
- Do not treat ray tracing as the goal.
- Do not blur all normals globally to fix grazing lighting.
- Do not let blood, sweat or dirt silently invalidate the calibrated base
  material — that is what the residual validity term is for.
- Do not optimize only for close-ups.
- Do not ignore the roughness and normal changes mipmapping causes.
- Do not let different light types produce different material semantics. This is
  the one that costs the most to unpick later.

## 11. Assets

`tools/get_head_scan.sh [scan|head|face|all]`

- **scan** (default) — RenderPeople's free "Dennis" sample: a photogrammetry
  human, ~100k triangles, 8K diffuse and 8K tangent-space normal. The
  high-fidelity subject; the pore-level normal map is what actually exercises the
  dual specular lobe, the normal split and the mip behaviour. Ships as OBJ, so
  `tools/obj_to_glb.py` converts it to metres, facing −Z.
- **head** — the Lee Perry-Smith head (Infinite-Realities, CC-BY 3.0), 1K maps.
  Small and instant; the head most published skin work is shown on.
- **face** — the MPFB / MakeHuman example avatar (CC0). Lower fidelity, but the
  only one of the three with separate eyeball, teeth and tongue meshes.
  **Known issue:** it carries morph targets, and the viewer's morph-instance
  path currently hangs on it and presents a corrupt frame. This reproduces on a
  clean tree, so it predates the character work; the bench does not auto-pick
  it. Until that is fixed, the eye and mouth materials are exercised by the
  bench's procedural stand-in — run `--demo lookdev` with no head asset present,
  or `RX_LOOKDEV_SUBJECT=` pointed at nothing.

`tools/wire_gltf_textures.py` embeds loose maps into a glTF whose material ships
with none, which is the usual state of a free scan.
