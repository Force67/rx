# rx::editor: the editor itself, as a library a game links into its own editor
# app (apps/editor is the default one: a main.cc over this module). Editing
# state (undo/redo, selection), the ugui interface, terrain and surface
# placement tools, import and world baking. Reflection, the transform hierarchy
# and .rxscene io live in rx::scene, because a game needs them without the
# editor.
rx_add_module(editor
  editor_app.cc
  editor_terrain.cc
  editor_ui.cc
  editor_input.cc
  selection.cc
  undo.cc
)

target_link_libraries(rx_editor PUBLIC
  rx::app rx::ui rx::terrain rx::render rx::anim rx::gltf rx::usd rx::blend rx::asset
  rx::scene rx::ecs rx::window rx::events rx::foundation
  # Bake World: the same cook rxworld runs, so an archive from either is the same bytes.
  rx::world)

target_compile_definitions(rx_editor PRIVATE
  RX_EDITOR_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui"
  RX_BLEND_CONVERTER_SCRIPT="${PROJECT_SOURCE_DIR}/tools/blend_to_glb.py")

if(RX_BUILD_TESTS)
  # Editing model test: reflection, scene round-trip, undo/redo and hierarchy
  # composition. Fully headless (no GPU), so it always runs under plain ctest.
  add_executable(edit_test edit_test.cc)
  target_link_libraries(edit_test PRIVATE rx::editor rx::scene rx::ecs rx::asset rx::foundation)
  add_test(NAME edit_test COMMAND edit_test)
endif()
