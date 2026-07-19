# Procedural Grass

`render::ProceduralGrass` reconstructs a deterministic blade field on the GPU
from compact semantic data. Applications describe where grass can grow, define
up to eight blade families, and submit bounded local interactions. The renderer
generates and draws only the candidates relevant to the current camera.

![Procedural grass across semantic hills](docs/images/procedural-grass-field.png)

## Inputs

Set `FrameView::grass_domain` for each frame that should render grass. The
domain and all pointed-to arrays are non-owning and must remain valid through
`Renderer::RenderFrame`.

| Type | Purpose |
| --- | --- |
| `GrassFieldSample` | One heightfield texel: height, density, discrete type, and growth multiplier |
| `GrassType` | Blade color, dimensions, curve shape, material response, wind response, and clump scale |
| `GrassSurfaceTriangle` | A growable authored-mesh triangle with a stable ID, density, growth, and type |
| `GrassInteraction` | A spherical local displacement, either radial or projected from a supplied direction |
| `GrassGenerationSettings` | Independent candidate spacing, streaming, density LOD, geometry LOD, fade, slope, and capacity controls |

The heightfield is optional when the domain contains surface triangles. This
supports overlapping or non-heightfield geometry such as rocks, roofs, and
placed soil meshes without changing the terrain representation.

```cpp
render::GrassDomain grass;
grass.samples = field.data();
grass.sample_width = field_width;
grass.sample_height = field_height;
grass.origin_x = world_origin.x;
grass.origin_z = world_origin.z;
grass.extent_x = world_extent.x;
grass.extent_z = world_extent.z;
grass.types = types.data();
grass.type_count = static_cast<u32>(types.size());
grass.surfaces = growable_triangles.data();
grass.surface_count = static_cast<u32>(growable_triangles.size());

view.grass_domain = &grass;
view.grass_interactions.push_back(interaction);
```

## Pipeline

1. `Prepare` sanitizes controls and copies the semantic field, blade families,
   surface triangles, and interactions into the current frame slot.
2. The compute pass resets counters, evaluates a camera-centered candidate
   grid plus mesh-surface candidates, and rejects roots by density, slope,
   distance, and a conservative point-frustum test.
3. Accepted roots are compacted into a fixed-capacity instance arena. Stable
   cell and surface IDs seed all jitter, dimensions, orientation, tint, and
   Voronoi clump behavior.
4. One indirect draw expands each instance into a cubic-Bezier ribbon. Nearby
   blades use seven curve segments; geometry LOD smoothly collapses those
   vertices onto a four-sample curve at distance.
5. Grass participates in the reversed-Z depth/motion prepass and the opaque
   scene pass. Wind deformation is evaluated at current and previous times to
   produce motion vectors.

Height, density, and growth interpolate continuously. Type IDs remain discrete:
stochastic interpolation softens boundaries without creating invalid blended
families. Density reduction, curve complexity, and distance fade are separate
controls so content can tune them independently.

## Controls

| Setting | Effect |
| --- | --- |
| `candidate_spacing` | World-space root spacing before semantic density rejection |
| `stream_tile_size`, `stream_radius` | Camera snapping and active generation radius |
| `density_lod_start/end`, `far_density` | Gradual distant candidate thinning |
| `geometry_lod_start/end` | Seven-to-three segment curve simplification range |
| `fade_start/end` | Blade height fade before the stream boundary |
| `max_slope_cos` | Minimum accepted surface-normal Y component |
| `max_blades` | Per-frame compacted blade limit |

`SanitizeGrassSettings` clamps unsafe values and preserves ordered LOD ranges.
The renderer feature switch is `RenderSettings::procedural_grass`; it can also
be set in a render preset as `procedural_grass` or overridden with
`RX_PROCEDURAL_GRASS`.

## Growable Meshes

![Grass generated on the demo's growable stone](docs/images/procedural-grass-surface.png)

Split growable mesh regions into `GrassSurfaceTriangle` records. Winding
defines the growth normal, area determines the candidate count, and
`surface_id` plus the local candidate index gives each root stable randomness.
Keep IDs stable while an object is loaded to avoid visible reshuffling.

## Limits

- Heightfields are capped at 256 by 256 samples.
- A domain accepts up to eight blade types, 2,048 surface triangles, and 16
  interactions.
- Generation is capped at 1,048,576 candidates and 262,144 blades per frame.
- GPU arenas are allocated lazily on the first valid grass domain.
- Vulkan grass pipelines require at least 224 bytes of push-constant storage;
  the optional feature disables cleanly on adapters below that limit.
- Surface candidate lookup is linear in submitted triangle count; merge small
  coplanar regions or submit only nearby growable surfaces for large worlds.
- Grass currently receives direct sun and ambient lighting but does not cast or
  sample the renderer's shadow systems.
- Grass is raster-only and is not included in path-traced frames.

## Demo

```sh
vkrun ./build/linux/runtime/rx --demo grass --no-rt
```

The scene combines a semantic rolling heightfield, three blade families,
stochastic family boundaries, a winding density-masked path, grass on a
standalone stone mesh, gusting wind, and a moving local interaction marker.
`RX_GRASS_SPACING` overrides candidate spacing for density stress tests, and
`RX_GRASS_MAX_BLADES` overrides the compacted blade cap.

## Layout

- `engine/render/geometry/procedural_grass.{h,cc}`: public data model, resource
  ownership, compute scheduling, and indirect draws.
- `engine/render/shaders/geometry/procedural_grass*`: candidate generation,
  shared ABI, curve expansion, prepass, and scene shading.
- `runtime/demo_grass.{h,cc}`: the `--demo grass` semantic field and growable
  surface example.

## Testing

- `procedural_grass_test`: settings sanitation, candidate-area calculations,
  and rejection of degenerate or non-finite surface triangles.
- `settings_ini_test`: feature-toggle serialization, parsing, round-trip, and
  partial-preset behavior.
