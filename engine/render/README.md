# engine/render - hygiene & contribution rules

Read this before touching anything in this directory. It exists so changes -
human or LLM - keep the renderer portable, data-driven and verifiable. For the
backend abstraction itself (device model, D3D12 mapping, HDR presentation),
read `RHI.md`; this file is about how to *add and modify* renderer code
without degrading it.

## Map

```
rhi/          public backend-agnostic API (the ONLY thing passes may use)
vulkan/ d3d12/ null/   backends; each translates rhi/ in its own directory
core/         renderer orchestration, render graph, bindless, settings, presets
pipeline/     opaque geometry path: mesh pipeline, materials, culling, meshlets
gi/           shadows, DDGI, ReSTIR, path tracers, denoisers, RT context
screenspace/  raster fallbacks: SSAO, SSR, SSGI, contact shadows
atmosphere/   sky, clouds, froxel fog, weather, IBL
geometry/     water, ocean FFT, hair, fur, particles, imposters, WBOIT
post/         TAA, upscalers, framegen, bloom, exposure, DoF, motion blur, VRS
texturing/    virtual texturing
shaders/      HLSL, mirrored per subsystem; rhi_bindings.hlsli is the ABI glue
util/         gpu profiler, EXR/screenshot, shader utilities
presets/      quality tier definitions
```

## Iron rules

1. **No graphics-API types outside the backends.** No `Vk*`, `D3D12*`, DXGI or
   SPIR-V type may appear in a pass, system or public header. Everything goes
   through `rhi/`. If the RHI can't express what you need, extend the RHI
   (types + every backend + null), don't bypass it. The only exception is the
   interop escape hatch (`rhi/vulkan_interop.h`) for modules wrapping
   API-specific SDKs (NRD, DLSS, FSR3) - and those must degrade gracefully
   when the backend doesn't match.
2. **Nothing above the RHI touches synchronization.** No fences, semaphores or
   hand-rolled barrier logic in passes. Inside the render graph, barriers are
   *derived from your declared accesses* - declaring them correctly IS your
   synchronization (see below).
3. **Pipelines are created once, in `Initialize`.** Never create pipelines,
   samplers or persistent resources per frame. Samplers come from
   `device.GetSampler({...})` (cached, never destroyed by callers).
4. **Every feature is toggleable and cap-gated.** New passes get a
   `RenderSettings` field and/or a `RX_*` env option, and must check device
   caps (ray query, mesh shaders, ...) before adding themselves to the graph.
   The renderer must render a correct (if plainer) frame with your feature
   off, on the null backend, and on a device without your required caps.
5. **Both backends or neither.** A change that only works on Vulkan is not
   done. If it genuinely can't run on d3d12 yet (e.g. `vk::RawBufferLoad`),
   it goes on the `RX_SHADER_NO_DXIL` list with a comment, and caps
   gating must keep the path unreached there.
6. **Verify with the golden harness before declaring victory** (see
   Verification). "It compiles" is not evidence for a renderer.

## Adding a render pass

Copy the shape of an existing pass - `screenspace/ssao.*` is the canonical
compute pass; `gi/recon_path_tracer.*` shows ray query + imported history +
shared bindless set; `core/bindless.*` shows a persistent set.

The pattern is always:

```cpp
class FooPass {
 public:
  struct Settings { ... };                      // tunables, sane defaults
  bool Initialize(Device& device);              // create pipelines ONCE
  void Resize(Device& device, Extent2D extent); // cache extents, resize owned history
  void Destroy(Device& device);                 // destroy exactly what you created
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle inputs..., ...);
 private:
  Settings settings_;
  PipelineHandle pipeline_;
};
```

- The push-constant struct lives in an anonymous namespace in the `.cc`, is
  padded explicitly, and must match the HLSL `PUSH_CONSTANTS` block
  field-for-field. Keep push blocks lean: Vulkan guarantees only 256 B total
  across all bound stages, and on d3d12 anything > 64 B spills to an upload
  ring.
- `AddToGraph` creates its outputs via `graph.CreateTexture`, declares every
  read and write in the setup lambda, records in the execute lambda via
  `BindPipeline` → `BindTransient` → `Push` → `Dispatch2D`/`BeginRendering`,
  and returns the output handle. Capture by value in the execute lambda
  (it runs after setup returns).
- Wire-up: member in `Renderer`, `Initialize`/`Destroy`/`Resize` calls,
  `AddToGraph` at the right spot in the frame, a `RenderSettings` toggle,
  and the `.cc` added to `CMakeLists.txt`.

## Render graph rules

`core/render_graph.h` is small - read it. The non-negotiables:

- **Declare every access** with the usage that matches how the shader actually
  touches it: `kSampledCompute` vs `kSampledFragment` vs `kSampledTaskMesh`
  are distinct sync scopes, not synonyms. An image sampled by a fragment pass
  but declared `kSampledCompute` produces a barrier that doesn't cover the
  fragment stage - it will render correctly on your machine and corrupt on
  another. If a resource is consumed by both stages in different passes, each
  pass declares its own correct usage.
- **Transient textures do not survive the frame.** Every `Acquire` starts
  `kUndefined` and the first barrier discards contents. Anything temporal
  (TAA history, ping-pongs, accumulation) is an owned `GpuImage` entering via
  `ImportImage(name, image, &state_member)` - the graph reads and writes back
  the tracked `ResourceState` so next frame's import is correct. Never cache a
  `ResourceHandle` or transient image across frames.
- **Async passes** (`builder.Async()`) manage their own barriers and must not
  touch any resource used by main-queue passes inside the overlap window; the
  first consumer declares `JoinAsync()`. Don't mark a pass async without
  understanding that window.
- Manual `cmd->Barrier(...)`/`MemoryBarrier(...)` is for code *outside* the
  graph (uploads, owned multi-dispatch internals), not a patch for accesses
  you didn't declare.
- `ClearColor`/`FillBuffer` require the image/buffer in `kCopyDst` state and
  created with transfer-dst usage.

## Adding or changing a shader

- HLSL only, compiled by dxc, dual-target (SPIR-V + DXIL sidecar). Every
  resource carries **both** `[[vk::binding(N, S)]]` and
  `: register(<class>N, spaceS)`, and push blocks use
  `PUSH_CONSTANTS(T, name)` from `shaders/rhi_bindings.hlsli`. Missing either
  annotation breaks one backend silently.
- Register the file in `RX_RENDER_SHADERS` in `CMakeLists.txt`, then
  reference it as `RX_SHADER(k_<name>_<stage>_hlsl)` - this wraps SPIR-V and
  DXIL as one `ShaderBlob`; pass code never branches on backend.
- `vk::RawBufferLoad` / buffer-device-address reads have no DXIL equivalent:
  the shader goes on `RX_SHADER_NO_DXIL` (with the existing comment
  style) and the feature must be caps-gated so d3d12 never reaches it. Prefer
  descriptor-based access unless BDA is genuinely needed.
- Includes resolve via `RX_SHADER_INCLUDE_DIRS`; wrapper shaders that
  `#include` a sibling need a `RX_SHADER_DEPS_<name>` entry so ninja
  rebuilds them.
- Binding slots declared in the pipeline desc must match the HLSL exactly -
  there is no reflection safety net.

## Settings, toggles, debug views

- Runtime tunables go in `RenderSettings` (`core/settings.h`). The renderer
  diffs settings against applied state each frame - make your feature react
  to its toggle flipping at runtime, including releasing resources when
  disabled. Expensive transitions go through a device idle (see how upscaler
  swaps do it).
- Env/config knobs are `base::Option` entries at the top of `renderer.cc`,
  named `RX_<FEATURE>` - follow the existing naming. No raw `getenv` calls.
- Quality tiers live in `presets/`; a new cost-relevant feature should be
  placed in the tiers, not just default-on.
- Debugability is part of the feature: if your pass produces an intermediate
  worth eyeballing, consider a `DebugView` entry (mirrored in
  `mesh.ps.hlsl`) - and give pipelines a `debug_name` always.

## Verification (do this, in order)

1. Build with both backends: the nix dev shell provides vkd3d, so
   `RX_RHI_D3D12` defaults ON. Configure **only inside
   `nix develop`** - a host-compiler-contaminated cache SIGBUSes.
2. Golden-image regression:
   `nix develop -c python3 tests/golden/golden.py --runner vkrun`.
   After an *intentional* visual change, regenerate refs with `--update` and
   commit them - and say so in the commit subject.
3. Cross-backend spot check for anything touching shared shaders or the RHI:
   `vkrun env RX_RHI=d3d12 ./build/nix/runtime/rx --demo materials --no-rt`
   should match Vulkan pixel-near-identically (see RHI.md for the method).
4. One-off screenshots: `RX_UI_SHOT=/tmp/shot.png RX_UI_SHOT_FRAMES=45`;
   `RX_FIXED_DT` makes time-driven systems deterministic.
5. Toggle matrix: your feature off, on, and on-without-caps (e.g. `--no-rt`)
   must all render clean frames - no validation errors under `swrun`
   (lavapipe + validation layers) where it runs.

## Pitfall log (hard-won; don't re-learn these)

- `proj.m[5]` is **negative** (Vulkan y-flip). Anything deriving NDC/screen
  scale from the projection matrix must account for it - see the
  `proj_scale` convention in `ssao.h`.
- Images written by compute and later read by fragment (or vice versa) need
  the *correct declared usages* on both sides; narrow barriers here are the
  classic "works on my GPU" bug.
- Push-constant blocks: 256 B hard ceiling; layouts with pushes at multiple
  offsets (shadow cascades at 0, per-draw at 64) must keep offsets stable -
  the d3d12 backend shadows them.
- Vendor-SDK view/resource rings (FSR3 ffx) advance **per dispatch**, not per
  frame - dispatch counts must match ring expectations.
- `BlitMip` on d3d12 lowers to a draw; don't assume it's a copy.
- The null backend must keep building - it's the interface-conformance check.
  If you extend the RHI, extend `null/` too.

## What NOT to do (LLM guardrails)

- Don't add a Vulkan include, `getenv`, or ad-hoc barrier "just to fix it
  locally" - fix the declaration or the RHI.
- Don't restructure `renderer.cc` pass ordering casually; passes have data
  and state dependencies the graph names but ordering intent lives here.
- Don't invent new file layout: pass = `subsystem/foo.{h,cc}` +
  `shaders/subsystem/foo.*.hlsl`, matching names.
- Don't delete or weaken existing comments explaining constraints - they are
  the pitfall log in situ.
- Don't regenerate golden refs to make a failing diff pass unless the visual
  change was the point of the patch.
- Match the surrounding style: `rx::render` namespace, `k`-prefixed
  constants, `f32/u32` core types, terse constraint-stating comments, no
  comment narration of the obvious.
