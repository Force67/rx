rx_add_module(scene
  camera.cc
  camera_rig.cc
  world_streaming.cc
  scene_handlers.cc
)

# scene depends on the script substrate (not the reverse): its handlers call
# ecs::World and the scene components directly, so no gateway indirection.
target_link_libraries(rx_scene PUBLIC rx::core rx::asset rx::ecs rx::script)
