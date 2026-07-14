rx_add_module(scene
  camera.cc
  camera_rig.cc
)

target_link_libraries(rx_scene PUBLIC rx::core rx::asset rx::ecs)
