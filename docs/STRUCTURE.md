# Code structure

Proposal, 2026-09-28. The code counterpart to the content layout in
[CONFIG.md](CONFIG.md). Modeled on Chromium's tree, a foundation (`base/`)
plus one folder per product (`content/`, `chrome/`) with layering inside a
folder enforced by the build, and Unreal's plugin and project model on top.

## Goals

- Every module builds, links and tests on its own.
- Layering is enforced by the build, not by this document.
- In development every module is its own shared library, so an edit relinks one
  `.so`/`.dll`. Shipping builds stay static. The same code builds both ways.
- Games and third-party plugins build against the engine exactly the way
  first-party code does.
- No module is a grab-bag. There is no `core`, no `runtime`, no `util`.

## Vocabulary

| term | meaning |
|---|---|
| module | A directory whose `CMakeLists.txt` calls `rx_module()`. One target, one DSO under `RX_SHARED`, one namespace (see Namespaces). A subdirectory without its own `rx_module()` is internal organization of the enclosing module. |
| plugin | A directory of one or more modules that an app opts into. First-party plugins live in `plugins/`, a game's own in its own `plugins/`. |
| app | An executable under `apps/`: the shell, the editor, a game, a tool. Apps never depend on each other. |

"Component" is not used for build units (Chromium's word) because it already
means an ECS component here.

## Layout

Four top-level folders, one per layer, each depending only on the ones above it
in this list:

```
foundation/           what rx adds to equilibrium base (Chromium: base/); one module
  build_config/       types.h, export.h
  logging/
  strings/            StrFormat, ToString, TextReader, TextWriter
  files/              fs, platform paths
  tasks/              job system
  memory/             arenas, pools, tracker, allocator override, Shared
  features/           feature flags (each module declares its own)
  time/               frame timer
  system/             device detection (Steam Deck, gamescope), app identity
  math/               vectors, matrices, Min/Max/Clamp
  algorithm/          StableSort, NthElement

rxe/                  the engine (Chromium: content/)
  gpu/                one module: the RHI and the backends Device::Create picks from
    rhi/              the interface, shader loading, gpu profiler
    vulkan/  d3d12/  null/   selected by RX_RHI_VULKAN / RX_RHI_D3D12
  ui/
    window/           window + backends: window_sdl3.cc, window_android.cc, wayland_kde_hdr.cc
    events/           input, actions, bindings
    ugui/             libultragui integration + the splash plate
    imgui/            rx::imgui_renderer: imgui draw lists through the RHI, + theme
  storage/            vfs, .rxp archives, the install layout mounts
  asset/              asset database and the runtime formats: mesh, texture, material, skeleton, collision shape
  importers/
    gltf/  usd/  materialx/  blend/
  audio/              device, mixer, codecs, spatialization, synth voices
  net/
    transport/        sessions, wire protocol, rpc channel
    http/             http, tls, url
    rpc/              rpc messages, registry, values
  physics/            physics world, shapes, cloth, water waves
  anim/               poses, rig player, anim graph, morphs, expressions, IK
  render/             the renderer and its shaders (see "render" below)
  render2d/           sprites, tile maps, iso
  ecs/
  script/
  scene/              scene components, cameras, .rxscene io, reflection, transform hierarchy, script handlers
  world/              world format, map, streaming, overlays, claims
  world_bake/         offline world baking (tools and the editor link it, games do not)
  devtools/           the command bridge and endpoint (rxcall, agents)
  app/                Host, Application, AppConfig, platform config, world clock
  editor/             the editor itself: selection, undo, panels, editor ui
  resources/          everything in rxe://: fonts/, config/

plugins/              optional, enabled per app (Chromium: components/)
  character/  combat/  inventory/  locomotion/  placement/  terrain/
  nav/                nav/ + nav_debug/ (the only half that knows the renderer)
  replication/        replication/ + replication_debug/
  vehicles/           cars, boats, aircraft, kites: physics, profiles, engine audio
  weather/            weather state, thunder synth

apps/                 every executable, one directory each (Chromium: chrome/, content_shell)
  shell/              the reference host, today's viewer
    demos/            the demo scenes and the feature gym
    scenes/  shaders/
  editor/             the default editor: main.cc, rxe/editor + every first-party plugin
  body_jiggle/        today's examples/
  rxpack/  rxworld/  rxdiff/  rxcall/
  <game>/             an in-tree game

tools/                scripts only: checkincludes, get_*.sh, *.py
testing/              test harness, renderer check, feature gym tour, shared test data
third_party/  cmake/  docs/
```

Includes are spelled from the repository root, as in Chromium:
`#include "rxe/physics/physics_world.h"`, `#include "foundation/logging/log.h"`,
`#include "plugins/nav/nav/agent.h"`. The top folder in every include tells
the reader which layer they are reaching into.

`rxe/` and `rxe://` are the same thing on purpose: `rxe/resources/` is exactly
what `rx_engine.rxp` packs and `rxe://` mounts.

## Layering

| folder | may depend on |
|---|---|
| `foundation/` | equilibrium base, third_party |
| `rxe/` | `foundation/` |
| `plugins/` | `foundation/`, `rxe/`, and plugins it names |
| `apps/` | everything above; never another app |

Inside `rxe/` the order comes from the modules' declared dependencies, which
must be acyclic, as inside Chromium's `content/`. Three rules are checked on top
of that:

- **Entity-free modules.** `gpu`, `ui`, `storage`, `asset`, `importers`,
  `audio`, `net`, `physics`, `anim`, `render` and `render2d` may not depend on
  `ecs`, `scene`, `script`, `world` or `app`. They work on their own handles, so
  they can be tested without an entity world, and `scene`/`app` are the only
  places that bind them to entities. This already holds today; the rule keeps it.
- **Nothing depends on `app`.** It is the engine's composition root: the only
  module that knows every subsystem, with no feature logic of its own.
- **Nothing a shipping game must link depends on `editor`, `world_bake` or
  `importers/usd`.** An app gets them only by asking.

## Module anatomy

```
rxe/physics/
  CMakeLists.txt
  README.md                 one paragraph: what this module hides
  physics_world.h           public: dependents include it
  physics_world.cc
  physics_world_test.cc     tests sit next to the code, named after the unit
  internal/                 private headers; the include lint rejects them from outside
```

```cmake
rx_add_module(physics physics_world.cc kinema_jolt.cc cloth_collision.cc)
target_link_libraries(rx_physics PUBLIC rx::foundation rx::asset)
target_link_libraries(rx_physics PRIVATE $<BUILD_INTERFACE:Jolt>)

if(RX_BUILD_TESTS)
  add_executable(cloth_collision_test cloth_collision_test.cc)
  target_link_libraries(cloth_collision_test PRIVATE rx::physics rx::foundation)
  add_test(NAME cloth_collision_test COMMAND cloth_collision_test)
endif()
```

- A module's tests are registered in its own `CMakeLists.txt`, below the
  module. Plain `add_executable` blocks, not a wrapper: tests differ in guards,
  shaders and properties, and a wrapper would have to grow every one of them.
- The folder a module lives in fixes what it may depend on. The include lint
  fails the test suite when a dependency points to a later folder.
- Configure writes the target graph to `build/<preset>/modules.json`
  (`cmake/module_graph.cmake`).
  `tools/checkincludes` runs as a ctest. Every quoted include must resolve to
  the module itself or a module in its dependency closure, and never to another
  module's `internal/`. The three `rxe/` rules above are checked here too. CMake
  stays the single source of truth; there are no separate DEPS files to drift.
- Exports use `RX_<MODULE>_EXPORT`, which `rx_add_module` defines; the
  `RX_DSO_*` primitives it expands to live in `foundation/build_config/export.h`.
- Platform and backend variants are files with suffixes selected by the build:
  `_sdl3.cc`, `_android.cc`, `_vulkan.cc`, `_d3d12.cc`. Native Vulkan calls
  outside `gpu/vulkan` (the NRD, DLSS and FSR3 glue, ugui's backend) live only
  in `_vulkan.cc` files.
- GPU renderer tests carry the `renderer` label and exit 77 (a CTest skip) when
  the hardware is missing. `vkrun ctest -L renderer` runs them for real; plain
  `ctest` reports them as skipped rather than passed.

## Foundation

`foundation` is one module, like Chromium's `base`: one target, one DSO, with a
subdirectory per topic. `net` linking the window code is not a risk here,
because the window code leaves `core` for `rxe/ui`; what stays is small and used
nearly everywhere. Nothing in it may include `rxe/`.

| subdirectory | today |
|---|---|
| `build_config` | `core/types.h`, `core/export.h` |
| `logging` | `core/log` |
| `strings` | `core/format`, `core/text_reader`, `core/text_writer` |
| `files` | `core/file_system`, `core/paths` |
| `tasks` | `core/job_system` |
| `memory` | `core/memory/*`, `core/shared.h` |
| `features` | `core/feature_registry`. `features.def` is gone (5b): a module declares each flag as a `base::Feature` in the file that reads it, so adding a flag edits no shared file. `InitFeatures` applies `RX_FEATURES` over the one process-wide chain. |
| `time` | `core/frame_timer` |
| `system` | `core/platform`, `core/app_identity` |
| `math` | `core/math.h`, `core/scalar.h` |
| `algorithm` | `core/sort.h` |

The rest of `core` goes into `rxe/`: `window*` and `wayland_kde_hdr` to
`ui/window`, `input*` to `ui/events`, and `world_clock` to `app` (simulation
time is not foundation).

## Mapping of the other modules

Paths on the right are under `rxe/` unless they start with `plugins/` or `apps/`.

| today | new |
|---|---|
| `render/rhi` | `gpu/rhi` |
| `render/vulkan`, `render/d3d12`, `render/null` | `gpu/vulkan`, `gpu/d3d12`, `gpu/null` |
| `render/util/shader_util`, `render/util/gpu_profiler` | `gpu/rhi` |
| `render/util/imgui_renderer`, `imgui_theme.h` | `ui/imgui` |
| `render/util/exr_write` | `asset` (next to `image_file`) |
| `ui` | `ui/ugui` |
| `asset/vfs`, `pack`, `content_mounts` | `storage` |
| `importers/gltf`, `usd`, `materialx`, `blend` (split out of `asset` in 2b) | same. tinyusdz, cgltf and stb stay private to the modules that use them. |
| rest of `asset` | `asset` |
| `audio/*` minus the game synths | `audio` |
| `vehicles` (cars, boats, aircraft, kites and their audio, gathered from `physics` and `audio` in 2b) | `plugins/vehicles` |
| rest of `physics` | `physics` |
| `net/protocol`, `wire.h`, `rpc_channel`, `znet_util.h` (target `rx_net`) | `net/transport` |
| `net/bubble`, `replication`, `session` (target `rx_replication`) | `plugins/replication/replication` |
| `net/bubble_debug` | `plugins/replication/replication_debug` |
| `http` | `net/http` |
| `rpc` | `net/rpc` |
| `app/*` | `app` (unchanged: `app::Host`, `app::Application`, `app::AppConfig`) |
| `ecs`, `script`, `anim`, `render2d` | same name |
| `scene/*`, `edit/reflect`, `edit/scene_io`, `edit/hierarchy` | `scene` (done in 4d-1). Loading a scene and composing world transforms are not editor features: the shell and `--validate` need them. |
| `edit/selection`, `undo` | `editor` |
| `apps/editor/*` minus `main.cc` | `editor` |
| `world/*` minus `world_bake` | `world` |
| `world/world_bake` | `world_bake` |
| `authoring` | `devtools` |
| `engine/assets/fonts`, `config/` | `resources/fonts`, `resources/config` |
| `character`, `combat`, `inventory`, `locomotion`, `placement`, `terrain`, `weather` | `plugins/<same>` |
| `nav/*` minus `nav_debug` | `plugins/nav/nav` |
| `nav/nav_debug` | `plugins/nav/nav_debug` |
| `runtime/*` | `apps/shell/`. `demo_*`, `scene_hook_*`, `placement_demo_assets` and `feature_gym/` go to `apps/shell/demos/`. |
| `runtime/scenes`, `runtime/shaders` | `apps/shell/scenes`, `apps/shell/shaders` |
| `apps/editor/main.cc` | `apps/editor/main.cc` |
| `examples/body_jiggle.cc` | `apps/body_jiggle/` |
| `assets/` (sponza, usd, openpbr downloads) | stays at the root: 1.7 GB, untracked, fetched by `tools/get_*.sh`; moving it would strand every existing download |
| `test/*_test.cc` | next to the unit they test, e.g. `test/aircraft_test.cc` becomes `plugins/vehicles/aircraft_test.cc` |
| `test/data`, `test/shaders`, `tests/renderer`, `tests/feature_gym` | `testing/` |
| `tools/rx*.cc` | `apps/rx*/main.cc` |

## render

`render` stays one module in this plan. Its subdirectories are internal
organization, renamed to say what they hold:

| today | new |
|---|---|
| `core/` | `renderer/`: renderer, render graph, bindless, dynamic resolution, presets, settings |
| `pipeline/` | `meshes/` (mesh pipeline, meshlets, gpu cull, virtual geometry, instance store) and `materials/` (material system, human and hair materials) |
| `geometry/` | `water/` (water, adaptive water, ocean fft, fluid sim, caustics, water field, shore wetting), `hair/` (grooms, strands, fur), `particles/`, `foliage/` (procedural grass, imposters), `splats/` (gaussians), `transparency/` (wboit) |
| `gi/` | `lighting/` (ddgi, rcgi, restir, light grid, shadows, sdf) and `raytracing/` (path tracer, reconstruction, rt instance cull, slot tracker, skinned rt, denoisers) |
| `atmosphere/`, `post/`, `screenspace/`, `texturing/`, `shaders/` | unchanged |
| `util/` | dissolved, see the mapping above |

The real problem is `renderer/renderer.cc`: 8.1k lines that know every feature.
Splitting render into per-feature modules first needs a `RenderFeature`
interface that the render graph drives. That gets its own design note and is
the last phase here. Once it lands, `render/water` and the others can each
become a module, and water or hair can become a plugin.

## Application, plugins and apps

`rxe/app` keeps today's shape. An app implements `app::Application`
(`OnInitialize`, `OnFixedStep`, `OnUpdate`, `OnBuildView`, `OnFrameEnd`,
`OnShutdown`) and hands it to a `Host`. Identity (`id`, `name`, `title`) stays
in `AppConfig`, as in CONFIG.md.

A plugin is one or more modules under `plugins/` (rx's) or a game's own
`plugins/`, built with `rx_add_module` like any engine module. An app enables a
plugin by linking it; nothing else wires it in, because no plugin needs startup
hooks today: they are libraries the app calls. The day a plugin must register
something at startup (components for `--dump-schema`, scheduler systems, editor
panels), it gets one `RX_PLUGIN` entry point and a registry that holds only
registration surfaces, never runtime services. Not before: an entry point with
no caller is a guess at an API.

`rx_add_module` also defines the module's export macro (`RX_<NAME>_EXPORT`) as a
compile definition, so a module outside rx needs no line in any file rx owns,
and the install package ships it (`find_package(rx)` brings `rx_add_module`).

`apps/shell/engine_context.h` (`EngineContext`) is deleted. Each demo takes the
services it uses as arguments.

### A game project

An out-of-tree game has the same shape as rx itself, minus the engine:

```
mygame/
  CMakeLists.txt        add_subdirectory(rx) or find_package(rx)
  apps/mygame/          the game: main.cc (composition root), the Application, gameplay
  apps/mygame_editor/   optional: the editor with the game's modules linked in
  plugins/<name>/       project plugins, the same shape as plugins/ in rx
  assets/  config/      content, as in CONFIG.md
```

```cmake
add_subdirectory(plugins/grapple)                 # rx_add_module(grapple ...)
add_executable(mygame apps/mygame/main.cc)
target_link_libraries(mygame PRIVATE rx::app rx::grapple rx::character rx::nav)
```

### The editor

The editor is engine code: `rxe/editor` holds it all, and `apps/editor` is the
default editor, a `main.cc` over `rx::editor`. A game extends it rather than
forking it: `apps/mygame_editor/main.cc` is the same few lines, linking
`rx::editor` plus the game's own modules, so the editor runs the game's code
without loading anything at runtime.

Third-party plugins are source plugins built against the engine with the same
toolchain and equilibrium version, as in Unreal. A binary-stable plugin ABI is
out of scope.

## Build modes

| preset | `RX_SHARED` | use |
|---|---|---|
| `linux-dev` (`build/linux-dev`) | ON | day-to-day work: one DSO per module |
| `linux`, `windows`, `android`, ... | OFF | shipping, benchmarks, install/export |

Benchmarks and captures keep running on the static presets, so the shared build
never skews a performance number.

What has to exist once per process, and how the shared build keeps it so:

- `base::Option` / `base::Feature` registry: equilibrium's `InitChain` and its
  item types carry default visibility, so the chain root is one ELF-unique
  symbol however many DSOs instantiate it. Before, an option set from a
  platform tier's `[options]` was invisible outside the DSO that declared it.
- volk: it cannot be shared (its table uses the Vulkan entry point names, which
  collide with SDL3's), so each DSO keeps a private copy. `rx::gpu` fills its
  own at device creation; every other DSO fills its copy inside
  `gpu::GetVulkanHandles`, which every Vulkan interop caller already calls
  first.
- log sinks, the feature table, app identity and the memory tracker live in
  `rx_foundation` and are reached through exported functions; the allocator
  override is compiled into the executable only (`rx_enable_mimalloc`).
- Windows: PE has no unique symbols, so a `windows-dev` preset first needs
  equilibrium's base linked as a DLL and the shared CRT (`/MD`) everywhere. Not
  done yet.

## Violations to fix before moving files

| where | problem | fix |
|---|---|---|
| `engine/core` | grab-bag | done (3): `foundation/`, `engine/ui/window`, `engine/ui/events`, and `world_clock` into `app` |
| `nav/nav_debug` | debug drawing needs the renderer | already its own target (`rx_nav_viz`); becomes `plugins/nav/nav_debug` |
| `net/bubble_debug` | debug drawing needs the renderer and Vulkan interop | already its own target (`rx_net_viz`); becomes `replication_debug` |
| `net/session`, `replication`, `bubble` | the net module knew the entity world | done (2a): split into `rx_net` and `rx_replication` |
| `physics/shape_desc.h` | inventory, physics-free by design, used it without linking physics | done (2a): plain data, moved to `asset/shape_desc.h` |
| `weather/weather.h` | used render's value headers without linking render | done (2a): links `rx::render`. Splitting render's value types into a light module waits for phase 7. |
| `audio/*_synth`, `vehicle_audio` | game audio inside the audio module | done (2b): vehicle audio to `engine/vehicles`, the thunder synth to `weather` |
| `physics/aircraft` etc. | vehicles inside the physics module | done (2b): `engine/vehicles` |
| `asset/usd_loader` etc. | every asset user links tinyusdz | done (2b): `engine/importers/*` |
| `render/util` | grab-bag | done (4c): `gpu/rhi`, `asset/exr_write`, `ui/imgui` |
| `runtime/engine_context.h` | god object passed to every demo | phase 6, with the rest of `runtime/` |
| `fly_camera` | duplicated in `runtime/` and `apps/editor/` | done (2b): `scene::FlyCamera`, fed a resolved `FlyCameraInput` |
| `core/features.def` | one file every feature edits | done (5b): flags live where they are read |
| `CMakeLists.txt` | 1381 lines, registers every test centrally | done (4b): each module registers its own |
| `anim/locomotion` vs `engine/locomotion` | two modules called locomotion | done (2b): `anim::ProceduralGait` |

## Phases

Each phase is one PR and changes no behavior. Verification is the build, `ctest`,
`vkrun ctest -L gpu`, and `rxdiff` on the shipped scenes at 20+ frames before
and after.

1. **Lint.** `tools/checkincludes` reads the graph CMake already has (sources
   and `target_link_libraries`), so no build file changes. Today's violations
   sit in `tools/checkincludes/baseline.txt`; the ctest fails on a new one and
   on a baseline line that was fixed but not deleted. Done.
2. **Fix the violations** in the table above, without moving directories.
   2a empties the lint baseline: the `net`/`replication` split, `ShapeDesc`
   into `asset`, weather linking render, and CPU tests linking the module they
   test instead of compiling its sources in. 2b moves what a mechanical
   `git mv` cannot: `vehicles` and `importers` out of `physics`, `audio` and
   `asset`, one `FlyCamera`, and the `ProceduralGait` rename. The rest of the
   table lands in the phase named in its row.
3. **Foundation.** Split `core` into `foundation/*`, `ui/window`, `ui/events`.
   Done: `foundation/` already sits at its final place, `rx::window` and
   `rx::events` wait in `engine/ui/` for phase 4.
4. **Move the tree**, one PR per step, each a `git mv` plus the include rewrite
   tool:
   - 4a (done): `engine/` becomes `rxe/` and `plugins/` (module names and
     namespaces unchanged), replication gets its own plugin folder, includes
     are spelled from the repository root, and `engine/assets` plus `config/`
     become `rxe/resources/`.
   - 4b (done): tests move next to their code and register in their module's
     `CMakeLists.txt`; shared fixtures go to `testing/data`, the renderer gate
     and feature gym tour to `testing/`. The top-level `CMakeLists.txt` drops
     from 1400 lines to 600.
   - 4c (done): the grouping inside `rxe/`: `gpu/` out of `render` (one
     module, since `Device::Create` constructs every backend), `net/transport`,
     `net/http`, `net/rpc`, `ui/ugui`; `render/util` dissolves into `gpu/rhi`,
     `asset` and `ui/imgui`.
   - 4d-1 (done): `authoring` becomes `devtools`, and reflection, `.rxscene`
     io and the transform hierarchy move from `edit` into `scene`. `app` keeps
     its name. What stays in `edit` (selection, undo) becomes `editor` in
     phase 6, with `apps/editor`.
   - 4d-2 (done): flat foundation, `internal` for every `detail`, `rx::rpc`
     into `rx::net`, and `rx::importers`, `rx::vehicles`, `rx::weather`,
     `rx::replication` matching their folders.
   - 4d-3 (done): the RHI into `rx::gpu` (backends `rx::gpu::vk`,
     `rx::gpu::d3d12`, `rx::gpu::null`); window, events and the imgui renderer
     into `rx::ui`; `WorldClock` into `rx::app`, `exr_write` into `rx::asset`.
     The include lint now checks namespaces too.
5. **Development shared build.**
   - 5a (done): the whole tree links as shared objects (missing exports
     added), one option registry per process, per-DSO volk filled through
     `GetVulkanHandles`, and the `linux-dev` preset.
   - 5b (done): per-module feature flags on the single registry;
     `features.def` and its three never-read render flags are gone.
6. **Plugins and apps.**
   - 6a (done): `runtime/` becomes `apps/shell/`, the tools `apps/rxpack`,
     `rxworld`, `rxdiff`, `rxcall`, and `examples/` `apps/body_jiggle`.
   - 6b (done): a module outside rx builds like one inside: `rx_add_module`
     defines the export macro (the central table in `export.h` and the two
     local `export.h` workarounds are gone) and ships in the install package.
     `RX_PLUGIN`, `PluginRegistry` and `rx_add_app` wait for a plugin that
     needs startup registration; none does today.
   - 6c: `rxe/editor` and a thin `apps/editor`, per-game editors.
   - 6d: `EngineContext` goes away; the demos move to `apps/shell/demos` in
     `rx::shell`.
7. **Renderer features.** A separate design note for `RenderFeature`, then split
   `renderer.cc`.

Out-of-tree consumers (recreation, anyvoxel) break whenever a header moves.
`tools/rewrite_includes/renames.txt` records every include and symbol rename
since phase 2; after bumping rx they run
`tools/rewrite_includes/rewrite_includes.py <their source dirs>` once. Each phase
that moves a header appends to that file.

## Namespaces

| code | namespace |
|---|---|
| `foundation/` | flat `rx::` (`rx::StrFormat`, `rx::Min`, `rx::StableSort`, `rx::FrameArena`), as all of `base/` is `base::`. The one sub-namespace is `rx::fs`, mirroring `std::filesystem`. `rx::mem` went flat; its category API says `Memory` in the name instead (`MemoryCategory`, `RegisterMemoryCategory`), since the namespace no longer does. |
| `rxe/<folder>/*` | `rx::<folder>`: one namespace per top-level folder, as `net::` covers all of Chromium's `net/`. `gpu::Device`, `ui::Window`, `net::RpcValue`, `importers::LoadGltfScene`, `world::` for `world` and `world_bake`. One exception: `rxe/net/http` stays `rx::http`, because its API is named for the protocol (`http::Get`, `http::Url`, `http::Response`) and `net::Get` would lose that. |
| backends | nested under their interface: `rx::gpu::vk`, `rx::gpu::d3d12`, `rx::gpu::null` |
| `plugins/<name>/*` | `rx::<name>` (`rx::nav`, `rx::vehicles`) |
| first-party apps | `rx::shell`, `rx::editor` |
| third-party plugins, games | their own root, never `rx::`, as Chromium components use `autofill::` and not `chrome::` |

Renames: `rx::authoring` becomes `rx::devtools`, `rx::rpc` becomes `rx::net`,
and `rx::edit` splits into `rx::scene` and `rx::editor`.

- The folder fixes the namespace; `tools/checkincludes` checks that every
  header under `rxe/` and `plugins/` opens it. A block of another `rx`
  namespace in the same header may only forward-declare that namespace's types
  (`namespace rx::gpu { class Device; }`).
- Private helpers live in `internal` (matching the `internal/` directories);
  file-local ones in a `.cc` in an anonymous namespace. `detail` and
  `format_detail` go away.
- No `using namespace`, in headers or `.cc` files. `using rx::render::Renderer;`
  is fine.
- Types that repeat their namespace (`physics::PhysicsWorld`) are not renamed in
  bulk. New names avoid the repetition; old ones change when their module moves.
