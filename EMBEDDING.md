# Embedding rx in a game

rx is consumed two ways: from source with `add_subdirectory` (the primary flow,
used by the reference consumer [recreation](https://github.com/Force67/recreation)),
or as a pre-built package via `install()` + `find_package(rx)`
(see [Install / find_package](#install--find_package)).

## CMake integration

```cmake
# your-game/CMakeLists.txt
set(MYGAME_RX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../rx" CACHE PATH "rx checkout")
set(RX_BUILD_RUNTIME OFF CACHE BOOL "" FORCE)  # skip the rx viewer
set(RX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${MYGAME_RX_DIR} rx)

add_executable(mygame main.cc game.cc)
target_link_libraries(mygame PRIVATE rx::app)
```

rx keeps its cmake helpers off `CMAKE_MODULE_PATH` (they are included by
path), so a consumer's own `cmake/warnings.cmake` and friends are never
shadowed and no module-path juggling is needed around the `add_subdirectory`.

Requirements: CMake 3.24+, a C++23 compiler, and dxc (DirectXShaderCompiler)
on PATH - shaders are HLSL compiled to SPIR-V at build time and embedded into
the render library, so shipped binaries carry no shader files.

## Module targets

Every module is a static library with an `rx::` alias; link only what you use.
Dependencies form a strict DAG - nothing links upward.

| target | what it is | depends on |
|---|---|---|
| `rx::core` | window/input/jobs/math/log/clock | equilibrium only |
| `rx::ecs` | entity storage + stage scheduler | core |
| `rx::asset` | glTF/MaterialX/primitives/vfs | core |
| `rx::rpc` | value/registry layer | core |
| `rx::scene` | transform/renderable components (header-only) | core, asset |
| `rx::render` | the renderer behind the RHI | core, asset |
| `rx::physics` | Jolt-backed rigid bodies | core, asset |
| `rx::anim` | pose + locomotion helpers | core, asset |
| `rx::audio` | mixer + decoders + SDL output | core, asset |
| `rx::app` | composition root: subsystem bringup + frame loop | all of the above |

Headers are included module-qualified from one root: `#include
"render/core/renderer.h"`, `"core/log.h"`. Avoid top-level directory names in
your own include root that collide with the module names above.

## Dependency provider contract

rx brings its own third-party dependencies unless the parent project already
provides the targets, checked with `if(NOT TARGET ...)`:

- `volk` / `VMA` / `Vulkan::Headers` - fetched (pinned tags) when absent.
- `equilibrium::base` - from rx's `third_party/equilibrium` submodule when
  absent (`git submodule update --init` in the rx checkout).
- `kinema` - the skeletal animation runtime, a sibling checkout of
  [Force67/kinema](https://github.com/Force67/kinema) when absent
  (`RX_KINEMA_DIR` overrides the `../kinema` default).
- `SDL3` - `find_package` (QUIET); without it the window/audio backends
  compile to headless stubs. `RX_FETCH_SDL3=ON` downloads it instead.

Optional GPU SDKs (FSR3, DLSS, NRD, Jolt) are not in the tree; run the
`tools/get_*.sh` scripts inside the rx checkout, or the matching `RX_*`
options silently turn off. Forwardable options: `RX_FSR3`, `RX_DLSS`,
`RX_NRD`, `RX_JOLT`, `RX_AUDIO_FFMPEG`, `RX_SANITIZE`, `RX_MIMALLOC`.

## The app framework

`rx::app` is the layer a game embeds instead of forking the viewer. The
`app::Host` owns every subsystem (window, jobs, world clock, ECS
world/scheduler, renderer, physics, vfs, audio, input map) and runs the loop:
fixed-step ECS stages for simulation, then per drawn frame a FrameView
assembled from every `Transform`+`Renderable` entity (with motion-vector
history) and submitted to the renderer. The game implements
`app::Application` and receives the hooks between those steps:

```cpp
#include "app/application.h"
#include "app/host.h"

class MyGame : public rx::app::Application {
 public:
  bool OnInitialize(rx::app::Services& s) override {
    services_ = &s;                // engine services, stable for the run
    // load content, register ECS systems, spawn entities ...
    return true;
  }
  void OnFixedStep(rx::f32 dt) override { /* gameplay at fixed dt */ }
  void OnUpdate(rx::f32 dt) override { /* input -> camera, UI begin */ }
  void OnBuildView(rx::f32 dt, rx::render::FrameView& view) override {
    view.camera.eye = eye_;        // the app owns the camera
    view.camera.target = target_;  // extra draws/lights/UI also go here
  }
 private:
  rx::app::Services* services_ = nullptr;
  rx::Vec3 eye_{0, 2, 6}, target_{0, 1, 0};
};

int main() {
  rx::app::AppConfig config;       // renderer desc + quality preset + headless
  MyGame game;
  rx::app::Host host;
  if (!host.Initialize(config, game)) return 1;
  int rc = host.Run();             // or drive host.RunFrame() yourself
  host.Shutdown();
  return rc;
}
```

`runtime/` (the rx viewer) is the reference `Application`: demo scenes, fly
camera, scripted camera paths and the imgui overlay are all viewer policy
implemented on these hooks, not engine code.

Platforms that own the main loop (mobile) drive `host.RunFrame()` directly
and forward surface loss/recreation to `host.OnSurfaceDestroyed()` /
`OnSurfaceCreated()`.

## Install / find_package

As an alternative to `add_subdirectory`, rx can be built and installed once,
then consumed out-of-tree with `find_package(rx)`:

```sh
# build + install rx (inside its dev shell so the toolchain matches)
cmake -B build -S .
cmake --build build
cmake --install build --prefix /path/to/rx-prefix
```

```cmake
# your-game/CMakeLists.txt
find_package(rx REQUIRED)          # CMAKE_PREFIX_PATH=/path/to/rx-prefix
add_executable(mygame main.cc)
target_link_libraries(mygame PRIVATE rx::app)   # same rx:: targets as in-tree
```

Install rules are generated only when `RX_INSTALL` is on, which defaults to on
only when rx is the top-level project (`PROJECT_IS_TOP_LEVEL`). An
`add_subdirectory` consumer never triggers them, so that build is unchanged.

### Package layout

| path | contents |
|---|---|
| `include/` | rx public headers, module-qualified (`core/log.h`, `render/core/renderer.h`, ...) — same `#include "render/core/renderer.h"` style as in-tree |
| `include/rx-deps/` | bundled third-party public headers (equilibrium, Vulkan-Headers, volk, VMA) |
| `lib/` | the `rx_*` static archives |
| `lib/rx/` | bundled third-party static archives |
| `lib/cmake/rx/` | `rxConfig.cmake`, `rxConfigVersion.cmake`, `rxTargets.cmake` |

(`lib` is whatever `GNUInstallDirs` picks, e.g. `lib64`.)

### How the dependency closure is satisfied

rx's modules are static archives, and a static archive does **not** absorb its
dependencies — the final consumer's linker needs every archive in the closure.
rx's own modules go through `install(EXPORT)` with the `rx::` namespace. The
third-party closure can't ride the same export cleanly (FetchContent'd /
submodule / vendored targets whose include dirs live in read-only source
trees), so rx takes the pragmatic route: it **bundles those archives + their
public headers into the package** and **recreates them as imported targets in
`rxConfig.cmake`**, re-attaching them to the `rx::` targets. Concretely:

- `equilibrium::base`, `volk`, `Vulkan::Headers`, VMA — bundled archive/headers,
  recreated as imported targets (these are PUBLIC, their headers propagate).
- `kinema`, `Jolt`, and the optional `rx::ffx_fsr3` / `rx::dlss` / `rx::nrd`
  (+ `ShaderMakeBlob`) SDK archives — bundled, re-attached link-only.
- `Threads` and (when the build used it) `SDL3` — resolved with
  `find_dependency`.
- vkd3d (d3d12 backend) and wayland-client (KDE HDR) — re-resolved via
  `pkg-config` in the config, so the consumer must be able to reach those
  system libraries (it builds in the same environment rx was built in).
- The header-affecting PUBLIC defines on `rx::render` (`RX_RHI_VULKAN`,
  `RX_RHI_D3D12`, `RX_HAS_DLSS`, `RX_HAS_NRD`) ride the export automatically as
  usage requirements, and C++23 is propagated as a compile feature.

### Tradeoffs

- **The package is toolchain/platform specific.** It bundles archives built by
  a specific compiler/runtime and bakes pkg-config lookups; consume it from the
  same environment (the same dev shell) that built it.
- **`RX_FETCH_SDL3` builds are not installable** — a fetched (non-system) SDL3
  would need bundling too. Install builds should use a system SDL3 (found via
  `find_package`), which the config then recovers with `find_dependency(SDL3)`.
- The bundled third-party headers land under `include/rx-deps/` and are only on
  the include path of the rx targets that need them; treat them as private to
  rx, not as a general vendored-dependency SDK.

## RX_SHARED — shared-library mode (experimental, the "DLL test")

`-DRX_SHARED=ON` builds each rx module as a shared object (`librx_core.so`,
`librx_render.so`, ...) instead of a static archive. This is **not** a
distribution mode — the static build is what ships. It is a correctness
discipline: every module is compiled with hidden visibility (in *both* build
modes, so the annotations stay honest), and a symbol only crosses a module
boundary when its declaration carries that module's export macro from
`engine/core/export.h` (`RX_CORE_EXPORT`, `RX_RENDER_EXPORT`, ...). Building
shared forces the real cross-DSO API to be declared and flushes out ambient
process-global state that a single static link would paper over.

```sh
cmake -S . -B build/shared -G Ninja -DRX_SHARED=ON -DRX_INSTALL=OFF
cmake --build build/shared
ldd build/shared/runtime/rx | grep librx_   # links the 8 module .so's
```

Status and constraints:

- **Experimental.** Configuring with `RX_SHARED=ON` prints a warning. The
  Linux/GCC + Vulkan + vkd3d configuration on this tree builds and runs all
  demos; MSVC `__declspec(dllexport/dllimport)` branches exist in `export.h` but
  are untested.
- **`RX_SHARED` + `RX_INSTALL` is unsupported** and errors out at configure
  time: the install/export packaging bundles and re-imports *static* `rx_*`
  archives. `RX_INSTALL` defaults on only for a top-level rx, so pass
  `-DRX_INSTALL=OFF` (an `add_subdirectory` consumer already has it off).
- **`base::Option` env knobs are per-DSO.** equilibrium's option registry
  (`base::InitChain`) keeps its list head in a vague-linkage function-local
  static. Under hidden visibility that head is a *separate instance per shared
  object*, so `base::InitOptionsFromEnv()` called from one DSO only populates
  that DSO's options. rx handles this by re-applying env overrides inside each
  option-owning DSO at init (`Renderer::Initialize`, `AudioSystem::Initialize`,
  the viewer's `main`), all guarded by `RX_SHARED_BUILD` so the static build is
  unchanged. Net effect: `RX_WIN_W`, `RX_ASYNC_COMPUTE`, `RX_AUDIO_VOLUME`, etc.
  all work — but the mechanism differs from the static single-registry build,
  and a *new* option-owning DSO must apply its own overrides. `base::Feature`
  (the `features.def` registry in `rx::core`) is unaffected: it is a hand-rolled
  array reached only through exported `core` functions, so it is single-instance
  process-wide either way.
- **volk is one private copy per DSO.** volk's global function-pointer table is
  named identically to the real Vulkan entry points; exporting it would collide
  with SDL's own Vulkan symbols, and leaving it half-exported (volk's default)
  splits the filled table from the read table across DSOs. Under `RX_SHARED`
  volk is compiled fully hidden, so each DSO that uses it (`librx_render`, the
  viewer's imgui) gets a private, non-interposing copy that it initializes
  itself (render in `vk_device.cc`, the viewer in `debug_ui.cc`).

## Current limitations

- No public/private header split yet: every module header is reachable from
  every consumer. Treat anything outside the obvious module front doors
  (`renderer.h`, `world.h`, `vfs.h`, ...) as internal.
- `rx::render` links volk/VMA publicly; Vulkan headers propagate to render
  consumers.
