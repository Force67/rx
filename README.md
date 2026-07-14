# rx - render experience

A standalone real-time rendering engine, extracted from the
[recreation](https://github.com/Force67/recreation) project. rx is the part of
that engine that has nothing to do with Bethesda games: the Vulkan/D3D12
renderer, the asset pipeline, ECS, physics, animation and audio, plus a small
viewer runtime in place of the game.

## What's here

- **engine/render** - the renderer, behind a backend-agnostic RHI
  (`engine/render/rhi/`: vulkan, d3d12-via-vkd3d, null). HLSL shaders compiled
  to SPIR-V at build time with dxc. Feature set includes: TAA / MSAA /
  FSR3 / DLSS upscaling + FSR3 frame generation, hardware ray tracing (RT
  shadows/AO/reflections, DDGI, RCGI radiance-cache GI, ReSTIR DI), a
  reference path tracer, NRD
  denoising, screen-space SSS, strand hair, virtual textures, virtual geometry
  (cluster-DAG LOD, gpu-driven two-pass hi-z occlusion culling, 64-bit
  visibility buffer with compute + mesh-shader rasterization, instancing),
  froxel volumetrics, local shadow atlas, decals, lit
  translucency, FFT ocean, gaussian splats, GPU particles, HDR10 output,
  dynamic resolution, texture streaming, async compute, VRS, meshlet path.
- **engine/asset** - glTF loading (cgltf) including morph targets and weight
  animations, MaterialX, primitives, LOD simplification, Loop subdivision,
  virtual filesystem.
- **engine/core** - SDL3 windowing (+ KDE HDR monitor), job system, input
  action layer with gamepad support, math, logging, feature registry.
- **engine/ecs / scene / physics (Jolt) / anim / audio / rpc** - entity
  storage and scheduling, scene components, Jolt-backed rigid bodies, pose and
  locomotion helpers, a facial expression controller (damped per-region
  transitions between named morph poses, plus a blink/micro-motion life
  layer), an SDL mixer with wav/xwma decoding, and a small RPC value/registry
  layer. Skeletal animation sampling comes from
  [kinema](https://github.com/Force67/kinema), a reusable SoA animation
  runtime consumed as a sibling checkout.
- **engine/app** - the composition root a game embeds instead of forking the
  viewer: `app::Host` owns the subsystems and the fixed-step/render loop and
  drives a game-implemented `app::Application`. See [EMBEDDING.md](EMBEDDING.md)
  for using rx as the engine of your own game.
- **runtime/** - the `rx` viewer (the reference `app::Application`):
  `--gltf <scene>` or `--demo <id>` (water,
  materials, cornell, lod, oit, fire, bricks, sss, strands, vt, vgeo, lights,
  meshlet, occlusion, imposters, gaussian, fur, gpuparticles, autolod, mtlx),
  fly camera, imgui debug overlay (F1), physics cube toss (F), camera
  record/replay/orbit/showcase drivers (`RX_RECORD` / `RX_REPLAY` / `RX_ORBIT`
  / `RX_SHOWCASE`), frame capture (`RX_UI_SHOT`).

## Global illumination

Indirect diffuse comes from one of three tiers, all fully dynamic (rx bakes
nothing - no lightmaps, no probe bakes, no per-level GI data on disk):

- **SSGI** - screen-space bounce; the raster fallback when ray query is
  unavailable.
- **DDGI** (default) - a single camera-following probe volume, traced and
  blended every frame. Simple and cheap, but range-limited (its whole volume
  is ~24 m) and it only bounces sun + emissive.
- **RCGI** (`RX_RCGI=1`, experimental) - a cascaded radiance-cache pipeline
  modeled on the GI id Software shipped in idTech 8: a world-space cascaded
  light grid lights ray hits outside the frustum, hits land in a spatially
  hashed world radiance cache that is shaded incrementally (sun + emissive +
  clustered lights + previous-frame bounce), cascaded octahedral irradiance
  volumes (~256 m range, one cascade updated per frame) integrate the cache,
  and a half-res 1-ray/pixel final gather resolves per-pixel GI through a
  three-level cache fallback (previous frame's screen radiance, then the
  radiance cache, then the volumes) into 2-band SH, which a bilateral
  denoise + temporal upscale turn into the indirect-diffuse term. The why:
  DDGI's every-probe-every-frame update cannot scale its volume up, while
  RCGI's amortized caches keep a flat ~0.2 ms world-side cost regardless of
  range, add point/spot-light bounce, and the gather restores the per-pixel
  contact detail probe interpolation loses - for roughly +1 ms over DDGI.
  Design, GPU interfaces and measured costs: [RCGI.md](RCGI.md).

Both probe modes need ray query; the path tracer modes (`RX_PATHTRACE*`)
remain the ground-truth reference.

## Building

```sh
git submodule update --init          # third_party/equilibrium
git clone https://github.com/Force67/kinema ../kinema   # animation runtime (sibling)
tools/get_jolt.sh                    # physics (optional but recommended)
tools/get_fidelityfx.sh              # FSR3 (optional)
tools/get_nrd.sh                     # NRD denoiser (optional)
tools/get_dlss.sh                    # DLSS (optional, NVIDIA)
cmake --preset linux
cmake --build build/linux
build/linux/runtime/rx --demo cornell
```

Requirements: CMake 3.24+, a C++23 compiler, dxc (DirectXShaderCompiler),
SDL3. Vulkan headers/volk/VMA are pinned and fetched by CMake. On NixOS just
use the dev shell: `nix develop`, which also provides `vkrun` (host NVIDIA
driver bridging) and `swrun` (headless lavapipe + Xvfb software path).

```sh
tools/get_sponza.sh
build/linux/runtime/rx --gltf assets/sponza/Sponza.gltf
```

## Notes

- The C++ namespace is `rx::`; env-var knobs are `RX_*` (`RX_PATHTRACE=1`,
  `RX_DRS=1`, `RX_MSAA=4`, `RX_HDR_OUTPUT=pq`). Grep for `base::Option` to see
  the full set.
- `engine/render/presets/` holds editable .ini quality presets, loadable live
  from the debug overlay.
- History predating the extraction lives in the recreation repository.
