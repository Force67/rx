rx_add_module(edit
  selection.cc
  undo.cc
)

# Editor state over the scene: undo/redo and selection. Reflection, the
# transform hierarchy and .rxscene io live in rx::scene, because a game needs
# them without the editor. Deliberately NOT on rx::render (the editor UI links
# render separately and drives it through the FrameView contract).
target_link_libraries(rx_edit PUBLIC rx::foundation rx::ecs rx::scene rx::asset)

if(RX_BUILD_TESTS)
  # Editor foundations test: reflection, scene round-trip, undo/redo and hierarchy
  # composition. Fully headless (no GPU), so it always runs under plain ctest.
  add_executable(edit_test edit_test.cc)
  target_link_libraries(edit_test PRIVATE rx::edit rx::scene rx::ecs rx::asset rx::foundation)
  add_test(NAME edit_test COMMAND edit_test)
endif()
