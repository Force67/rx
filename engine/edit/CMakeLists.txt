rx_add_module(edit
  reflect.cc
  hierarchy.cc
  selection.cc
  scene_io.cc
  undo.cc
)

# Editor foundations: reflection, hierarchy, serialization, undo and selection.
# Depends on the data modules only; deliberately NOT on rx::render (the editor
# UI links render separately and drives it through the FrameView contract).
target_link_libraries(rx_edit PUBLIC rx::core rx::ecs rx::scene rx::asset)
