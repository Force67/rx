# Hair rendering

A production fibre BSDF — Marschner's R / TT / TRT lobes in Chiang's
parameterization — with Zinke dual scattering fed by a deep opacity map, shared
by strand grooms and hair cards, and shadowing the skin underneath.

## Why not a Kajiya-Kay highlight

What this replaced was two Kajiya-Kay power lobes with hardcoded constants, a
flat ambient fill, and no shadowing of any kind. It produced opaque straw:

| | before | after |
| --- | --- | --- |
| Fibre model | two power lobes on the tangent | R / TT / TRT + residual |
| Colour | a texture tint multiplied into the result | absorption, inverted from the target colour |
| Multiple scattering | none | dual scattering from a measured fibre count |
| Self-shadowing | none | deep opacity map |
| Shadow on the head | none | the same volume |
| Cost (1080p, 763k ribbon tris) | 0.24 ms | 0.32 ms draw + 0.24 ms volume |

A strand is a dielectric **cylinder**, and what makes hair read as hair is where
light goes after it enters one:

- **R** — surface reflection. The white sheen. Achromatic: it never entered the
  fibre, so it carries no pigment.
- **TT** — straight through. This is why backlit hair glows.
- **TRT** — in, off the back wall, out. The coloured secondary highlight and the
  glint.
- and in anything lighter than black, **most of what you see has bounced between
  many strands**. A blonde groom shaded with single scattering comes out looking
  like dark straw no matter what colour it is painted, which is the failure that
  usually gets "fixed" by painting the hair brighter — permanently breaking the
  link between its colour and how it scatters.

| Piece | Where |
| --- | --- |
| The BSDF + dual scattering | `engine/render/shaders/hair_bsdf.hlsli` |
| CPU side, presets, tiers, CPU mirror | `engine/render/pipeline/hair_material.{h,cc}` |
| Transmittance volume (deep opacity map) | `engine/render/geometry/hair_strands.{h,cc}`, `shaders/geometry/hair_dom.ps.hlsl` |
| Volume sampling, shared | `engine/render/shaders/geometry/hair_transmittance.hlsli` |
| Strand shading | `engine/render/shaders/geometry/hair.ps.hlsl` |
| Card shading | the `kFlagHair` branch in `shaders/pipeline/mesh.ps.hlsl` |
| Regression test | `test/hair_bsdf_test.cc` |

```sh
build/linux/runtime/rx --demo strands                    # loose hair, a braid, a ponytail
RX_DEBUG_VIEW=24 build/linux/runtime/rx --demo strands   # the fibre count
```

## One evaluator, cards and strands

`HairShade` is the only place hair is shaded. Strand grooms call it from
`hair.ps.hlsl`; hair cards call it from the `kFlagHair` branch of the forward
pass. A character whose card fringe and whose simulated strands shade
differently has two hair materials, and only one of them can be right.

Cards differ in exactly two respects, both stated at the call site: they take
`h` from the card's uv (which runs across the strand width) rather than from the
ribbon expansion, and they are not in the transmittance volume, so they assume
an authored fibre depth instead of measuring one — falling back to the volume
when a strand groom happens to be overhead.

## Colour is pigment, not a tint

Hair colour is two pigments: eumelanin (brown/black) and pheomelanin
(red/yellow). `HairSigmaFromMelanin` maps concentrations to absorption; the
shipped presets (black, brown, blonde, red, grey, white) are melanin
concentrations in the ranges measured in human hair.

Authoring in pigment keeps a groom's colour coupled to how it scatters. Painting
a fibre "blonde" by lowering its absorption also gives it the forward glow blonde
hair actually has. **Multiplying an albedo into the shaded result cannot do
this** — it produces blonde hair that scatters like brown hair, which is the
single most common way hair rendering goes wrong.

Artists still want to paint a colour, so the default mode inverts a target
colour into absorption instead. Which brings us to:

### The published inversion does not transfer, and this is how far off it is

Chiang's colour inversion is calibrated against **full path-traced multiple
scattering** — dozens of intra-fibre bounces. Measured against this renderer's
dual-scattering transport, it is badly wrong:

| asked for | Chiang's fit renders |
| --- | --- |
| 0.90 | 0.995 |
| 0.75 | 0.966 |
| 0.60 | 0.897 |
| 0.45 | 0.768 |
| 0.30 | 0.549 |
| 0.10 | 0.112 |

Everything light comes out washed toward white — visibly, an olive-khaki blonde
instead of a gold one.

So the mapping was **fitted against this renderer** rather than copied. Measuring
the absorption that actually renders as a given colour gives a strikingly simple
law:

```
sigma_a = -ln(c) / D,     D = 2.17 + 2.02 * n_ref
```

`n_ref` is the fibre depth at which the authored colour is exact. `D` turned out
to be **independent of the azimuthal roughness** (8.23 to 8.29 across
`beta_n` 0.15–0.75 at n = 3), which is why this is so much simpler than the
published polynomial — that form's roughness dependence is an artefact of the
transport it was fitted to, not a property of the fibre. Round-trip error is
under **0.002** over c in [0.07, 0.85] and n in [3, 10], and `hair_bsdf_test`
asserts it.

`HairSigmaFromColorPathTraced` keeps Chiang's constants, because they are the
right answer for a path tracer and because the discrepancy is worth being able
to reproduce.

`color_reference_depth` chooses *which part of the groom* you painted. Shallower
parts read lighter and deeper parts darker, which is what hair does.

## The transmittance volume

Self-shadowing, dual scattering and the shadow a groom casts on the forehead are
all the same question: **how many fibres are between this point and the light?**
A binary shadow map cannot answer it — hair is not opaque, and "in shadow / not
in shadow" turns a groom into a black cutout with a hard edge.

Deep opacity map (Yuksel & Keyser), two passes over the same ribbon geometry
from the sun:

1. **front depth** — the nearest fibre per light-space texel. No fragment
   shader; the depth is the whole output.
2. **layered counts** — additive, **depth test off**, because every fibre along
   the ray has to be counted including the ones the front one occludes. Four
   layers at 0.15 / 0.35 / 0.65 / 1.0 of the layer depth past the front surface.

Anchoring the layers to the front fibre rather than to a global slab is the
point: that is where the density gradient is steepest, and the difference
between one fibre and four is the difference between a lit rim and a shadowed
one. A ribbon fragment contributes `sqrt(1 - side²)` rather than 1, so a clump's
count is proportional to the hair actually in the way and not to how many quads
overlapped.

The stored quantity is a **count**, not an opacity, because dual scattering is
parameterized on fibres crossed.

The light frustum is fitted to the grooms' own bounds, so 1024 texels resolve
individual strands on a head.

`RX_DEBUG_VIEW=24` shows the count as a heat ramp, in the forward pass **and in
the strand pass** — a debug view occluded by the geometry it is diagnosing is not
a diagnostic.

### Hair shadowing the scene

The forward pass reads the same volume (env slots 44–46) and attenuates the sun
by `exp(-hair_shadow_density * fibres)`, for every material rather than only
skin: a groom shadows the collar and the shoulders too, and restricting it to
flagged materials is how a head ends up with a shadow that stops at the hairline.

The volume is built later in the frame than the forward pass reads it, so **the
scene-side shadow is one frame old**. Hair moves slowly relative to a frame and
the alternative is reordering the graph around it; the staleness is bounded and
the lag is invisible.

## Dual scattering

Zinke's split: light reaching a fibre has already passed through others
(**forward** scattering — attenuated and spread), and what the viewer sees also
includes light that came back out of the neighbours (**backward** scattering).

The exact formulation integrates the BSDF over the sphere per shading point.
This is the real-time reduction, with the approximations stated where they are
made:

- average forward/backward attenuations evaluated analytically from the fibre's
  own absorption, fitted against the single-scattering evaluation in the same
  file rather than taken from the paper's tables — so the two halves stay
  consistent when the BSDF changes;
- `T_f = a_f^n` evaluated in log space, so a groom hundreds of fibres deep does
  not underflow to a hard black core;
- lobe spread grows as `sqrt(n)` (the random-walk result), and the
  multiply-scattered term is evaluated against a deliberately blunted copy of
  the fibre — a highlight in the multiple-scattering term is a highlight that
  never survived the walk to get there;
- local back-scattering saturates rather than growing with depth: past a few
  fibres the extra ones are already dark.

The failure mode of dual scattering is that it looks plausible while being wrong
by a constant factor, which is why the test pins its *behaviour*: it must
attenuate with depth, lift a light fibre more than a black one, and be what makes
white hair white rather than grey.

## Controls

| Control | What it does | Safe range |
| --- | --- | --- |
| `beta_m` | longitudinal roughness — highlight width **along** the strand | 0.02 … 1 |
| `beta_n` | azimuthal roughness — highlight width **around** it | 0.02 … 1 |
| `alpha` | cuticle scale tilt, radians. Shifts R toward the tip and TRT toward the root; that separation is why a real highlight is two offset bands. Remove it and hair reads as plastic tubing. | 0 … 0.175 |
| `eta` | index of refraction. Keratin is 1.55. | 1.3 … 1.8 |
| `density` | groom fibre density multiplier for dual scattering | 0 … 4 |
| `scatter_scale` | gain on multiple scattering | 0 … 3 |
| `color_reference_depth` | the fibre depth at which an authored colour is exact | — |

Renderer settings: `hair_transmittance`, `hair_transmittance_depth` (metres the
four layers span; too small and the interior saturates in the first layer, too
large and the front fibres stop resolving), `hair_fibre_scale` (rendered ribbons
→ optical fibres), `hair_shadow_density`.

## Quality tiers

A reduction of the same BSDF, never a different one. The test asserts the
reduction is monotonic.

| | Hero | Standard | Distant |
| --- | --- | --- | --- |
| Lobes | R/TT/TRT + residual | same | same, broadened |
| Dual scattering | measured depth | constant depth | off |
| Transmittance volume | yes | no | no |
| Per-fragment `h` | yes | no | no |
| Cuticle tilt | yes | yes | no |

The distant tier broadens the lobes rather than keeping them: at that size a
narrow lobe is aliasing, not detail.

## What the tests pin

`test/hair_bsdf_test.cc`, on the CPU mirror:

1. **Energy.** A non-absorbing fibre reflects 0.93–1.02 of what it receives,
   integrated over the whole **sphere** (a cylinder scatters on every side),
   across roughness and incidence. Marschner's three lobes do not sum to one on
   their own — the residual term closes the gap. If that regresses, every
   light-coloured groom silently loses energy and comes out dark, which looks
   like an art problem and gets fixed by painting the hair brighter.
2. **The azimuthal geometry.** The R lobe peaks at `phi = -2*asin(h)`, to within
   0.01 rad. That single check pins the whole cylinder geometry, because
   `gamma_o`, `gamma_t` and `Phi_p` all fall out of it.
3. **Lobe identity.** R is achromatic even on a heavily pigmented fibre; TT
   carries the pigment's colour.
4. **Pigment coupling.** More eumelanin is always darker; pheomelanin reddens
   rather than merely darkening.
5. **The fitted colour inversion** renders the colour it was asked for, at every
   reference depth — and the published constants are confirmed to be far off, so
   nobody swaps them back in.
6. **Multiple scattering** attenuates with depth, lifts a light fibre more than a
   black one, and is what makes white hair white.
7. **Tier monotonicity.**

### Reciprocity, and why it is not asserted

This model is **not reciprocal**, and that is a property of the published
formulation rather than of this implementation: the per-lobe attenuations and the
internal refraction geometry are derived from the outgoing direction alone. The
asymmetry is measurable — tens of percent on the transmission lobes, and it does
not go away with a clear fibre, no tilt and `h = 0`.

It is accepted in production because the error sits on lobes that have already
been attenuated, and because the alternative is an order of magnitude more
expensive. It does mean this BSDF must not be dropped into a bidirectional
integrator that assumes reciprocity. The test **pins the magnitude** rather than
asserting the property away, so the asymmetry cannot grow unnoticed.

## Known gaps

- The volume covers strand grooms only. Hair cards fall back to an authored
  depth (they do cast ordinary shadow-map shadows, being regular geometry).
- No transparency ordering for the ribbons; they are drawn opaque against depth.
- Nothing here has been fitted against photographed hair. The presets are melanin
  concentrations from the literature and the roughnesses are reasoned, not
  measured. The colour inversion *is* fitted — but against this renderer, not
  against reality.
- The scene-side shadow is one frame old (see above).
