# Embedding rx in a game

rx is consumed from source with `add_subdirectory`; there is no installable
package yet. The [recreation](https://github.com/Force67/recreation) project
is the reference consumer.

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

## Current limitations

- No `install()`/`find_package` support; source builds only.
- No public/private header split yet: every module header is reachable from
  every consumer. Treat anything outside the obvious module front doors
  (`renderer.h`, `world.h`, `vfs.h`, ...) as internal.
- `rx::render` links volk/VMA publicly; Vulkan headers propagate to render
  consumers.
