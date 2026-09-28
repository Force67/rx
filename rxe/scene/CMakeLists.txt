rx_add_module(scene
  camera.cc
  camera_rig.cc
  fly_camera.cc
  reflect.cc
  scene_io.cc
  hierarchy.cc
  world_streaming.cc
  scene_handlers.cc
)

# scene depends on the script substrate (not the reverse): its handlers call
# ecs::World and the scene components directly, so no gateway indirection.
target_link_libraries(rx_scene PUBLIC rx::foundation rx::events rx::asset rx::ecs rx::script)

if(RX_BUILD_TESTS)
  add_executable(camera_test camera_test.cc)
  target_link_libraries(camera_test PRIVATE rx::scene rx::ecs rx::foundation)
  add_test(NAME camera_test COMMAND camera_test)

  add_executable(camera_rig_test camera_rig_test.cc)
  target_link_libraries(camera_rig_test PRIVATE rx::scene rx::ecs rx::foundation)
  add_test(NAME camera_rig_test COMMAND camera_rig_test)

  add_executable(world_streaming_test world_streaming_test.cc)
  target_link_libraries(world_streaming_test PRIVATE rx::scene rx::ecs rx::foundation)
  add_test(NAME world_streaming_test COMMAND world_streaming_test)
endif()
