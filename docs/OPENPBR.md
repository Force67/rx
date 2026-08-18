# OpenPBR Surface

rx imports [OpenPBR Surface](https://github.com/AcademySoftwareFoundation/OpenPBR)
v1.1.1, the Academy Software Foundation's standard surface shading model, from
both USD stages and standalone MaterialX documents, and shades the lobes it can
with the model the spec prescribes.

- USD: any material carrying an `ND_open_pbr_surface_surfaceshader`, through
  `asset::LoadUsdScene`
- MaterialX: `asset::LoadMaterialX("thing.mtlx", &material)` on an
  `open_pbr_surface` node
- Demo: `RX_MTLX=a.mtlx,b.mtlx ./build/linux/runtime/rx --demo mtlx`, one sphere
  per file. `tools/get_openpbr_samples.sh` fetches the upstream examples.

No new dependency. TinyUSDZ already parses the whole OpenPBR input set into
`tydra::RenderMaterial::openPBRShader` (`TINYUSDZ_WITH_USDMTLX`, on by default),
and the lobe math the spec asks for is analytic, including the Zeltner fuzz fits.

## What the spec is

A layered slab model rather than a list of lobes:

```
mix(ambient, surface, geometry_opacity)
  layer(coated-base, fuzz, fuzz_weight)
    layer(base-substrate, coat, coat_weight)
      mix(dielectric-base, metal, base_metalness)
        mix(opaque-base, translucent-base, transmission_weight)
          mix(glossy-diffuse, subsurface, subsurface_weight)
```

The spec defines a target appearance, not an implementation, and since 1.1.1
explicitly permits approximations. rx implements the "reduction to a mixture of
lobes" form the spec itself derives, which is what the layering above collapses
to under the albedo-scaling approximation.

## Coverage

| OpenPBR input | rx | notes |
| --- | --- | --- |
| `base_weight`, `base_color` | `base_color_factor` | weight folded into the colour |
| `base_diffuse_roughness` | `base_diffuse_roughness` | drives EON; 0 keeps Lambert |
| `base_metalness` | `metallic_factor` | |
| `specular_weight` | `specular_weight` | scales f0, and the metal Fresnel |
| `specular_color` | `specular_color` | dielectric tint / metal F82 edge tint |
| `specular_roughness` | `roughness_factor` | |
| `specular_ior` | `ior` | |
| `specular_roughness_anisotropy` | `anisotropy` | reparametrized, see below |
| `transmission_weight` | `transmission` | |
| `subsurface_weight`, `subsurface_color` | `subsurface`, `subsurface_color` | |
| `fuzz_weight`, `fuzz_color`, `fuzz_roughness` | `sheen_color`, `sheen_roughness` | folds onto the Charlie sheen lobe |
| `coat_weight` | `clearcoat` | |
| `coat_color` | `coat_color` | absorption tint, view dependent |
| `coat_roughness` | `clearcoat_roughness` | also roughens the base |
| `coat_ior` | `coat_ior` | |
| `coat_darkening` | `coat_darkening` | |
| `thin_film_weight` | `iridescence` | |
| `thin_film_thickness` | `iridescence_thickness` | micrometres to nanometres |
| `thin_film_ior` | `thin_film_ior` | |
| `emission_luminance`, `emission_color` | `emissive_factor` | product, in nits |
| `geometry_opacity` | `base_color_factor[3]` | |

Not imported, and dropped silently: the translucent-base volumetrics
(`transmission_color`, `transmission_depth`, `transmission_scatter`,
`transmission_scatter_anisotropy`), dispersion
(`transmission_dispersion_scale`, `transmission_dispersion_abbe_number`),
`subsurface_radius` / `subsurface_radius_scale` /
`subsurface_scatter_anisotropy` (rx's skin path has its own physical
parametrization, see `asset::Material::SkinParams`), `geometry_thin_walled`,
and the separate `geometry_coat_normal` / `geometry_coat_tangent`.

## Shading

In `engine/render/shaders/openpbr.hlsli`, used by `mesh.ps.hlsl` and
`mesh_rt.ps.hlsl`:

- **EON diffuse.** The Fujii form of Oren-Nayar plus the analytic reciprocal
  energy compensation of Portsmouth 2024, which is what makes it pass the white
  furnace test. At `base_diffuse_roughness == 0` it is exactly Lambert and the
  shader takes the cheap path, so nothing that does not author it changes.
- **F82-tint metal Fresnel** (Kutz 2021). Schlick, corrected to hit a chosen
  reflectivity at the ~82 degree grazing angle where real metals dip.
  `specular_color` sets that edge value as a fraction of Schlick, so white
  reduces it to plain Schlick exactly.
- **Coat.** `coat_ior` replaces a hardcoded 0.04 (1.5 reproduces it).
  `coat_color` absorbs along the refracted path in and out, so it darkens and
  saturates toward grazing angles. `coat_darkening` applies the internal
  reflection darkening from the spec's interfaced-Lambertian model. A rough coat
  roughens the base lobes by convolving the slope-space Gaussians.

Deliberate approximations:

- EON applies to the sun lobe. Punctual, area and image-based lighting stay
  Lambert, because the LTC and prefiltered-cube integrals assume a cosine lobe.
- The image-based path applies `coat_color` absorption, the coat darkening and
  the `specular_color` / `specular_weight` tint, so a tinted or coated surface
  reads consistently under ambient and sun. Two approximations there: indirect
  light arrives from the whole hemisphere, so the coat absorption path uses
  `ndv` for both legs where the direct path uses `ndv` and `ndl`; and the
  split-sum takes `specular_color` on its grazing (`f_ab.y`) half for metals,
  which is where F82's edge tint acts, rather than evaluating F82 under the
  integral.
- A rough coat now blurs the base for *any* material with a coat, including
  glTF `KHR_materials_clearcoat` content. At the glTF default
  `clearcoatRoughness` of 0 this is exactly a no-op, and it stays under 1% up
  to a coat roughness of ~0.1, but an authored rough coat shifts the base
  roughness materially (+32% at base 0.3 / coat 0.3). That is the physically
  correct direction, but it is a look change for existing content, not a
  purely additive one.
- Fuzz uses rx's existing Charlie/Ashikhmin sheen rather than the Zeltner
  microflake LTC the spec recommends, and is added over the base rather than
  albedo-scaled by `(1 - F * E_fuzz)`, so it adds a little energy.
- Fuzz sits *under* the coat, not above it. OpenPBR layers fuzz on top, so a
  coated fuzzy material has its fuzz wrongly dimmed and tinted by the coat.
  This is deliberate: the same lobe is glTF's `KHR_materials_sheen`, which
  glTF does layer beneath `KHR_materials_clearcoat`, and existing glTF content
  outnumbers coated-fuzz OpenPBR content. The two standards genuinely disagree
  here and rx cannot satisfy both with one lobe.
- GGX multiple-scattering compensation is not implemented, so rough metals and
  dielectrics are slightly dim and desaturated.
- Anisotropy: OpenPBR uses `a` in [0,1] with `alpha_b/alpha_t = 1 - a`, rx uses
  `ax = alpha*(1+k)`, `ay = alpha*(1-k)` with `k` in [-1,1]. The importers match
  the axis ratio with `k = a/(2-a)`, which reproduces the highlight shape but
  not the spec's `alpha_t^2 + alpha_b^2 = 2*alpha^2` mean-roughness
  normalization, so strongly anisotropic surfaces read a little rougher.
- Thin film keeps rx's cheap cosine interference model, not Belcour-Barla.
- The path tracer (`gi/pathtrace.cs.hlsl`) is diffuse-only and unchanged.

## Defaults

OpenPBR's defaults differ from glTF's, which rx's `asset::Material` follows so
that existing content is untouched. The two importers handle this differently
because they know different things:

- **MaterialX**: rx parses the XML itself, so it can tell an authored input from
  an absent one. `ApplyOpenPbrDefaults` seeds the spec values first, and only
  what the document actually authors overrides them.
- **USD**: `tydra::ShaderParam` carries a value and a texture id but no "was
  this authored" bit, so an unauthored input is indistinguishable from one
  authored to the same value. rx takes tydra's value as given rather than
  overriding it, since clobbering a real authored value is the worse failure.
  Some of tydra's fallbacks do disagree with the spec (`coat_roughness` 0.1 vs
  0, `subsurface_radius_scale` (1, 0.2, 0.1) vs (1, 0.5, 0.25), `thin_film_ior`
  1.5 vs 1.4), so a USD material that leaves those unauthored will not match a
  reference renderer.

One conversion is unconditional on both paths: the spec states
`thin_film_thickness` in micrometres, tinyusdz passes the document value through
verbatim into a field it documents as nanometres, so rx multiplies by 1000.

## Tests

- `test/openpbr_test.cc` covers the MaterialX mapping, the unit and
  anisotropy conversions, the seeded spec defaults, and that a legacy
  `standard_surface` document is not reinterpreted as OpenPBR.
- `test/usd_scene_test.cc` covers the USD path end to end on a stage it writes
  itself.

`tools/get_openpbr_samples.sh` pulls the 83 Apache-2.0 example materials from
the upstream repo into `assets/openpbr/` (gitignored). Point
`RX_OPENPBR_EXAMPLES` at that directory and `openpbr_test` additionally sweeps
every one of them, checking each parses and lands in the ranges the shader
relies on, plus two spot checks against values read out of the source documents:

```sh
tools/get_openpbr_samples.sh
RX_OPENPBR_EXAMPLES=assets/openpbr ./build/linux/openpbr_test
```

Of those 83, the inputs rx drops are exercised by a real fraction of the
corpus, so expect visible differences from a reference renderer on:
`subsurface_radius_scale` (16 materials), the dispersion pair (12),
`transmission_color` (7), `transmission_depth` (3), `geometry_thin_walled` (2),
and one each of `transmission_scatter`, `subsurface_radius`,
`subsurface_scatter_anisotropy` and `coat_roughness_anisotropy`.
