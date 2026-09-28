if(RX_JOLT)
  rx_add_module(physics
    cloth_collision.cc
    physics_world.cc
    kinema_jolt.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::foundation rx::asset)
  # PRIVATE static archives, needed at final link. Wrapped in BUILD_INTERFACE so
  # they stay out of the installed export; rxConfig.cmake re-adds bundled
  # imported equivalents for find_package consumers.
  target_link_libraries(rx_physics PRIVATE
    $<BUILD_INTERFACE:Jolt> $<BUILD_INTERFACE:kinema::kinema>)
  set(RX_INSTALL_JOLT ON PARENT_SCOPE)
else()
  # Without Jolt the world is a stub whose bodies are all invalid, so every
  # consumer (rx::vehicles, demo_drive.cc) still links and degrades to inert.
  rx_add_module(physics
    physics_world_stub.cc
    cloth_collision.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::foundation rx::asset)
endif()
