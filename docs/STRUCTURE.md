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
  gpu/
    rhi/              the interface, shader loading, gpu profiler
    vulkan/  d3d12/  null/
  ui/
    window/           window + backends: window_sdl3.cc, window_android.cc, wayland_kde_hdr.cc
    events/           input, actions, bindings
    ugui/             libultragui integration + the splash plate
    imgui/            imgui renderer + theme
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
  scene/              scene components, cameras, .rxscene io, reflection, scene script handlers
  world/              world format, map, streaming, overlays, claims
  world_bake/         offline world baking (tools and the editor link it, games do not)
  devtools/           the command bridge and endpoint (rxcall, agents)
  host/               Host, HostClient, HostConfig, platform config, world clock
  editor/             the editor itself: hierarchy, selection, undo, panels, editor ui
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
    scenes/  shaders/  assets/ (sponza, usd, openpbr downloads)
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
  `ecs`, `scene`, `script`, `world` or `host`. They work on their own handles, so
  they can be tested without an entity world, and `scene`/`host` are the only
  places that bind them to entities. This already holds today; the rule keeps it.
- **Nothing depends on `host`.** It is the engine's composition root: the only
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
rx_module(physics
  SOURCES      physics_world.cc kinema_jolt.cc cloth_collision.cc
  DEPS         foundation asset                           # public
  PRIVATE_DEPS jolt kinema
  TESTS        physics_world_test.cc
  GPU_TESTS    )                                           # labeled "gpu"
```

- The folder a module lives in fixes what it may depend on. Configure fails
  when a dependency points to a later folder or closes a cycle.
- Configure writes the target graph to `build/<preset>/modules.json`
  (`cmake/module_graph.cmake`).
  `tools/checkincludes` runs as a ctest. Every quoted include must resolve to
  the module itself or a module in its dependency closure, and never to another
  module's `internal/`. The three `rxe/` rules above are checked here too. CMake
  stays the single source of truth; there are no separate DEPS files to drift.
- Exports use `RX_<MODULE>_EXPORT` from `foundation/build_config/export.h`, as
  today.
- Platform and backend variants are files with suffixes selected by the build:
  `_sdl3.cc`, `_android.cc`, `_vulkan.cc`, `_d3d12.cc`. Native Vulkan calls
  outside `gpu/vulkan` (the NRD, DLSS and FSR3 glue, ugui's backend) live only
  in `_vulkan.cc` files.
- GPU tests carry the `gpu` label. `vkrun ctest -L gpu` runs them for real;
  plain `ctest` still skips them, but now visibly by label.

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
| `features` | `core/feature_registry`. `features.def` goes away: each module declares its own flags in its `features.h`, Chromium's `BASE_DECLARE_FEATURE` shape, so adding a flag no longer edits a shared file. |
| `time` | `core/frame_timer` |
| `system` | `core/platform`, `core/app_identity` |
| `math` | `core/math.h`, `core/scalar.h` |
| `algorithm` | `core/sort.h` |

The rest of `core` goes into `rxe/`: `window*` and `wayland_kde_hdr` to
`ui/window`, `input*` to `ui/events`, and `world_clock` to `host` (simulation
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
| `app/*` | `host`. `app::Application` becomes `HostClient` (a `Client` is the interface an embedder implements), `AppConfig` becomes `HostConfig`. |
| `ecs`, `script`, `anim`, `render2d` | same name |
| `scene/*`, `edit/reflect`, `edit/scene_io` | `scene`. Loading a scene is not an editor feature: the shell and `--validate` need it. |
| `edit/hierarchy`, `selection`, `undo` | `editor` |
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
| `assets/` (sponza, usd, openpbr downloads) | `apps/shell/assets/` |
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

## Host, plugins and apps

`rxe/host` keeps today's shape. An app implements `HostClient`
(`OnInitialize`, `OnFixedStep`, `OnUpdate`, `OnBuildView`, `OnFrameEnd`,
`OnShutdown`) and hands it to a `Host`. Identity (`id`, `name`, `title`) stays
in `HostConfig`, as in CONFIG.md.

A plugin module registers itself through one function:

```cpp
// plugins/nav/nav/nav_plugin.cc
RX_PLUGIN(nav, PluginRegistry& registry) {
  RegisterNavHandlers(registry.script_handlers());
  registry.scheduler().AddSystem(...);
}
```

`PluginRegistry` holds only things that accept registrations: script handlers,
reflected component types (which is what `--dump-schema` reads), scheduler
systems, devtools commands and editor panels. It holds no runtime services, so
it cannot grow into a god object. `rx_add_app` generates the table of enabled
plugins and the host calls it once at startup. There is no `dlopen` and no
reliance on static initializers, so a static link cannot drop a plugin
silently.

`apps/shell/engine_context.h` (`EngineContext`) is deleted. Each demo takes the
services it uses as arguments.

### A game project

An out-of-tree game has the same shape as rx itself, minus the engine:

```
mygame/
  CMakeLists.txt        add_subdirectory(rx) or find_package(rx); rx_add_app(...)
  apps/mygame/          the game: main.cc (composition root), the HostClient, gameplay
  plugins/<name>/       project plugins, the same shape as plugins/ in rx
  assets/  config/      content, as in CONFIG.md
```

```cmake
rx_add_app(mygame
  PLUGINS character combat nav        # rx's plugins/ or the game's, resolved by name
  EDITOR)                             # also build mygame_editor
```

### The editor

The editor is engine code: `rxe/editor` holds it all, and `apps/editor` is the
default editor, a `main.cc` that links `rxe/editor` with every first-party
plugin. A game extends it rather than forking it. `EDITOR` builds
`mygame_editor` from `rxe/editor`, the game's own modules and its plugins, so
the editor always runs the game's code without loading anything at runtime.
A plugin adds editor panels, tools and inspectors through `PluginRegistry`,
the same way it adds script handlers; a game that needs more than that writes
its own `apps/mygame_editor/main.cc` against `rxe/editor`.

`apps/shell` is `rx_add_app(rx PLUGINS <all first-party>)`. A tool is
`rx_add_app` with no plugins.

Third-party plugins are source plugins built against the engine with the same
toolchain and equilibrium version, as in Unreal. A binary-stable plugin ABI is
out of scope.

## Build modes

| preset | `RX_SHARED` | use |
|---|---|---|
| `linux-dev`, `windows-dev` | ON | day-to-day work: one DSO per module |
| `linux`, `windows`, `android`, ... | OFF | shipping, benchmarks, install/export |

Benchmarks keep running on the static presets, so the shared build never skews
a performance number.

Before `RX_SHARED` can be the default for development, anything that must exist
once per process has to live in exactly one DSO:

- the `base::Option` registry (today one instance per DSO, see the CMake
  warning): export it from `foundation` or build equilibrium base shared
- the allocator override (`memory/new_override.cc`) and mimalloc; on Windows,
  the shared CRT (`/MD`) everywhere
- log sinks, the feature list and the app identity
- volk: under `RX_SHARED`, volk is built as its own shared library so every
  `_vulkan.cc` file sees one loaded function table

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
| `render/util` | grab-bag | phase 4, when `gpu/` exists to receive shader loading and the profiler |
| `runtime/engine_context.h` | god object passed to every demo | phase 6, with the rest of `runtime/` |
| `fly_camera` | duplicated in `runtime/` and `apps/editor/` | done (2b): `scene::FlyCamera`, fed a resolved `FlyCameraInput` |
| `core/features.def` | one file every feature edits | phase 5: per-module flags self-register, which needs the one-per-process registry |
| `CMakeLists.txt` | 1381 lines, registers every test centrally | phase 4, with tests next to their code |
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
   - 4b: tests move next to their code (`rx_module(... TESTS)`).
   - 4c: the grouping inside `rxe/`: `gpu/` out of `render`, `net/` gathering
     `http` and `rpc`, `ui/ugui`; `render/util` dissolves into `gpu/rhi`,
     `asset` and `ui/imgui`.
   - 4d: namespace and module renames (`app` to `host`, `authoring` to
     `devtools`, `edit` split between `scene` and `editor`, the namespaces
     table below).
5. **Development shared build.** Add the single-instance fixes and the `-dev`
   presets, then per-module feature flags on the single registry.
6. **Plugins and apps.** Add `RX_PLUGIN`, `PluginRegistry` and `rx_add_app`.
   `runtime/` becomes `apps/shell/`, the tools move into `apps/`, and
   `EngineContext` goes away.
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
| `foundation/` | flat `rx::` (`rx::StrFormat`, `rx::Min`, `rx::StableSort`, `rx::FrameArena`), as all of `base/` is `base::`. The one sub-namespace is `rx::fs`, mirroring `std::filesystem`. `rx::mem` goes flat. |
| `rxe/<folder>/*` | `rx::<folder>`: one namespace per top-level folder, as `net::` covers all of Chromium's `net/`. `gpu::Device`, `ui::Window`, `net::Session`, `importers::LoadGltf`, `world::` for `world` and `world_bake`. |
| backends | nested under their interface: `rx::gpu::vk`, `rx::gpu::d3d12`, `rx::gpu::null` (today `rx::render::vk`, `rx::render::d`) |
| `plugins/<name>/*` | `rx::<name>` (`rx::nav`, `rx::vehicles`) |
| first-party apps | `rx::shell`, `rx::editor` |
| third-party plugins, games | their own root, never `rx::`, as Chromium components use `autofill::` and not `chrome::` |

Renames: `rx::app` becomes `rx::host`, `rx::authoring` becomes `rx::devtools`,
`rx::http`/`rx::rpc` become `rx::net`, and `rx::edit` splits into `rx::scene`
and `rx::editor`.

- `rx_module(... NAMESPACE gpu)` declares it; `tools/checkincludes` checks that
  every header of the module opens that namespace.
- Private helpers live in `internal` (matching the `internal/` directories);
  file-local ones in a `.cc` in an anonymous namespace. `detail` and
  `format_detail` go away.
- No `using namespace`, in headers or `.cc` files. `using rx::render::Renderer;`
  is fine.
- Types that repeat their namespace (`physics::PhysicsWorld`) are not renamed in
  bulk. New names avoid the repetition; old ones change when their module moves.
