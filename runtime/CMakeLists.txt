# The rx viewer: an app::Application on top of app::Host, adding the built-in
# demo scenes / glTF loading, the fly camera and the imgui debug overlay. The
# debug overlay compiles to a stub when imgui is absent, so the same
# translation units serve headless builds.
set(RX_RUNTIME_SOURCES
  viewer.cc
  camera_input.cc
  demo_scenes.cc
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
)

add_executable(rx main.cc ${RX_RUNTIME_SOURCES})
target_link_libraries(rx PRIVATE ${RX_RUNTIME_MODULES})
target_compile_definitions(rx PRIVATE RX_VERSION="${PROJECT_VERSION}")
rx_set_warnings(rx)
rx_enable_mimalloc(rx)

if(TARGET rx_imgui)
  target_link_libraries(rx PRIVATE rx::imgui)
  target_compile_definitions(rx PRIVATE RX_HAS_IMGUI=1)
  # Default location of the editable .ini render presets (engine/render/presets).
  # The debug ui lists and loads them; override with the env var RX_PRESETS_DIR.
  # Absolute so the dev build finds the source files.
  target_compile_definitions(rx PRIVATE
    RX_PRESETS_DIR_DEFAULT="${PROJECT_SOURCE_DIR}/engine/render/presets")
endif()
