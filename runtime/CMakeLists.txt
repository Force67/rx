# The rx viewer: an app::Application on top of app::Host, adding the built-in
# demo scenes / glTF loading, the fly camera and the imgui debug overlay. The
# debug overlay compiles to a stub when imgui is absent, so the same
# translation units serve headless builds.
set(RX_RUNTIME_SOURCES
  viewer.cc
  camera_input.cc
  viewer_input.cc
  demo_scenes.cc
  scene_hook_demo.cc
  scene_hook_rhi_demo.cc
  fly_camera.cc
  showcase_camera.cc
  debug_ui.cc
)

set(RX_RUNTIME_MODULES
  rx::app
  rx::core
  rx::ecs
  rx::asset
  rx::scene
  rx::audio
  rx::render
  rx::physics
  rx::anim
  rx::net
  rx::net_viz
)

add_executable(rx main.cc ${RX_RUNTIME_SOURCES})
target_link_libraries(rx PRIVATE ${RX_RUNTIME_MODULES})
target_compile_definitions(rx PRIVATE RX_VERSION="${PROJECT_VERSION}")
rx_set_warnings(rx)
rx_enable_mimalloc(rx)

# --demo scenehook: the app builds its own raw-Vulkan pipeline through the render
# interop, so its shaders embed here instead of in the render module. They read a
# buffer-device-address arena (RawBufferLoad/Store), a SPIR-V-only construct, so
# they cannot target DXIL (no d3d12 sidecar).
set(RX_SHADER_NO_DXIL
  scenehook_cull.cs.hlsl
  scenehook.vs.hlsl
  scenehook.ps.hlsl
  scenehook_rhi_cull.cs.hlsl
  scenehook_rhi.vs.hlsl
  scenehook_rhi.ps.hlsl
  scenehook_rhi.ms.hlsl
)
rx_embed_shaders(rx
  shaders/scenehook_cull.cs.hlsl
  shaders/scenehook.vs.hlsl
  shaders/scenehook.ps.hlsl
  shaders/scenehook_rhi_cull.cs.hlsl
  shaders/scenehook_rhi.vs.hlsl
  shaders/scenehook_rhi.ps.hlsl
  shaders/scenehook_rhi.ms.hlsl
)

if(TARGET rx_imgui)
  target_link_libraries(rx PRIVATE rx::imgui)
  target_compile_definitions(rx PRIVATE RX_HAS_IMGUI=1)
  # Default location of the editable .ini render presets (engine/render/presets).
  # The debug ui lists and loads them; override with the env var RX_PRESETS_DIR.
  # Absolute so the dev build finds the source files.
  target_compile_definitions(rx PRIVATE
    RX_PRESETS_DIR_DEFAULT="${PROJECT_SOURCE_DIR}/engine/render/presets")
endif()
