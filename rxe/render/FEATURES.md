# RenderFeature: design note

Status: proposed. Nothing here is built yet. Phase 7 of `docs/STRUCTURE.md`
split `renderer.cc` by concern; this note is what lets the frame itself come
apart, one feature at a time.

## Where the renderer is today

`core/renderer.cc` was one 8.1k-line file. It is now five:

| file | holds |
|---|---|
| `renderer.cc` | lifecycle: initialize, settings, upscalers, resize, swapchain, frame resources, shutdown |
| `renderer_upload.cc` | meshes, instance groups, hair grooms, imposters, textures, materials, decals |
| `renderer_frame.cc` | `RenderFrame`, `BuildFrameGraph`, the depth-only scene |
| `renderer_capture.cc` | screenshots, sequences, HDR dumps |
| `renderer_debug.cc` | debug lines, world text, picking |
| `renderer_internal.h` | the two things those files share beyond `renderer.h` |

That is a move, not a redesign: `BuildFrameGraph` is still one ~4.3k-line
function, and it is where every feature is known.

The subsystems themselves are already shaped alike. About fifty of them
(`WaterCaustics`, `ShoreWetting`, `OceanFft`, `DdgiSystem`, `BloomPass`, ...) have
`Initialize(device)`, `Destroy(device)`, `available()` and
`AddToGraph(graph, params)`. What keeps them welded to the renderer is the
glue around them, which lives in `BuildFrameGraph`. For one feature,
underwater caustics, that glue is:

1. **Activation.** `water_caustics_active_ = settings_.water_caustics &&
   water_caustics_.available() && !path_trace && !interior && scene_has_water`.
   Settings, availability and conditions about the frame, mixed.
2. **Frame globals.** It writes `globals.water_caustics[]` and sets
   `kFrameFlagWaterCaustics` so the scene shaders sample the map.
3. **Passes.** `water_caustics_.AddToGraph(graph_, params)`, placed by hand
   after the ocean pass because it reads the ocean's displacement.
4. **Outputs.** The opaque pass gets `water_caustics_.current_view()` as one
   positional argument of about thirty in `EnvironmentSystem::WriteEnvSet`, with a
   null view when the feature is off.

Plus the renderer member, the `RX_WATER_CAUSTICS` option in `renderer.cc`, the
settings field, and a `Destroy` in `Shutdown`. Adding a feature touches all of
these; removing one means finding them.

## The interface

```cpp
namespace rx::render {

// Built once per frame by the renderer, read by every feature.
struct FrameContext {
  const FrameView& view;
  const RendererSettings& settings;
  FrameGlobals& globals;       // features write their own fields and flags
  RenderGraph& graph;
  FrameOutputs& outputs;       // what features publish for later passes
  u32 frame_slot;
  bool path_trace;
  bool interior;
  // ...the other frame facts BuildFrameGraph derives today (scene_has_water,
  // render extent, jitter). Added as features need them, not up front.
};

class RenderFeature {
 public:
  virtual ~RenderFeature() = default;
  virtual const char* name() const = 0;
  virtual FeatureStage stage() const = 0;

  virtual bool Initialize(gpu::Device& device, BindlessRegistry* bindless) = 0;
  virtual void Destroy(gpu::Device& device) = 0;
  virtual void Resize(gpu::Extent2D render_extent) {}

  // Activation, globals and passes: steps 1 to 3 above. Active() is asked
  // once per frame; Prepare and AddPasses run only when it said yes.
  virtual bool Active(const FrameContext& ctx) const = 0;
  virtual void Prepare(FrameContext& ctx) {}
  virtual void AddPasses(FrameContext& ctx) = 0;
};

}  // namespace rx::render
```

`FeatureStage` is a short fixed list that orders what the render graph cannot
order by itself: `kSimulation` (ocean, water field, fluid, hair sim),
`kShadows`, `kLighting` (GI, reflections, clustered lights), `kTransparent`,
`kPost`, `kOverlay`. Within a stage features run in registration order. The
graph still derives barriers and culls unused passes from declared accesses;
stages exist for side effects the graph does not see (async compute, the first
writer of a persistent image).

`FrameOutputs` replaces step 4. A feature publishes a typed output; a consumer
asks for it and gets an empty value when the producer is off or absent:

```cpp
struct WaterCausticsOutput { gpu::TextureView map; };

outputs.Set(WaterCausticsOutput{water_caustics_.current_view()});   // producer
const auto caustics = ctx.outputs.Get<WaterCausticsOutput>();        // consumer
```

The type is the key, so a typo is a compile error, and a consumer does not
link its producer: the output struct lives in a small header the consumer
includes. This is what lets `WriteEnvSet`'s positional list shrink: each
argument becomes an output the environment pass reads.

## Ownership and registration

`Renderer` keeps `base::Vector<base::UniquePointer<RenderFeature>>` and
registers the built-in features in `InitializeCommon`. `Renderer::AddFeature`
takes one more before `Initialize`; that is how a plugin adds a feature, and it
needs no registry until a plugin needs startup registration (the same rule as
`RX_PLUGIN` in `docs/STRUCTURE.md`).

A feature owns its `RX_*` options (they move out of `renderer.cc` with it, as
feature flags did in phase 5b) and its entry in the GPU timing list.
`RendererSettings` keeps its fields for now: they are serialized by
`settings_ini` and set by the app, and moving them is its own change.

## What stays in the renderer

The spine of the frame is not a feature: visibility and culling, the depth and
scene passes, the lighting resolve, the post chain's fixed order, upscaling,
frame generation and present. The path tracer is a different frame, not a
feature of the raster one; it keeps its branch until it is migrated as a whole.
Features plug into the spine; the spine does not become a list of features.

## Migration

One feature per change, each proven with `rxdiff` at 20+ frames on the shipped
scenes (a migration must not move the picture). Features with one consumer
first, producers many passes read later:

1. `FrameContext`, `RenderFeature`, `FrameOutputs`, the feature list and
   stages, with `ShoreWetting` as the first feature (one output, one consumer).
2. `WaterCaustics`, then `OceanFft` (read by caustics, shore wetting and the
   scene pass).
3. The remaining water, then weather, hair, particles, foliage.
4. Once a directory's features are all migrated, it can become its own module
   (`rxe/render/water` as `rx_render_water`), and water or hair can move to
   `plugins/`. Render core never depends on them.

## Not doing

- No data-driven graph (a pass list in a file). The graph is built in code.
- No runtime reordering or per-scene feature lists beyond what settings do.
- No virtual call per pass or per draw: the interface is per feature per frame.
