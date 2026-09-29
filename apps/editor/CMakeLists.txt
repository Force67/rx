# The default rx editor: rx::editor (rxe/editor) on app::Host. A game builds its
# own editor the same way, linking its modules next to rx::editor.
add_executable(rx_editor_app main.cc)
# rx_editor is the rx::editor module; the binary keeps the name users run.
set_target_properties(rx_editor_app PROPERTIES OUTPUT_NAME rx_editor)
target_link_libraries(rx_editor_app PRIVATE rx::editor rx::app rx::foundation)
# An engine build: the tiers ship loose beside it, to be edited.
rx_stage_content(rx_editor_app LOOSE_ENGINE_CONFIG)
rx_set_warnings(rx_editor_app)
