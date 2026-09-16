# rx editor: an app::Application on app::Host with its entire UI in rx::ui
# (libultragui in draw-data mode). The ugui backend, platform shim and pipeline
# shaders live in engine/ui now, because the engine splash needs them too.

add_executable(rx_editor
  main.cc
  editor_app.cc
  editor_terrain.cc
  editor_ui.cc
  editor_input.cc
  fly_camera.cc)

target_link_libraries(rx_editor PRIVATE
  rx::app rx::ui rx::edit rx::terrain rx::render rx::anim rx::asset rx::scene rx::ecs rx::core
  # Bake World: the same cook rxworld runs, so an archive from either is the same bytes.
  rx::world)

target_compile_definitions(rx_editor PRIVATE
  RX_EDITOR_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
  RX_BLEND_CONVERTER_SCRIPT="${PROJECT_SOURCE_DIR}/tools/blend_to_glb.py")
