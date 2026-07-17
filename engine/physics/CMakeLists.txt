if(RX_JOLT)
  rx_add_module(physics
    cloth_collision.cc
    physics_world.cc
    vehicle_profiles.cc
    boat.cc
    boat_profiles.cc
    kinema_jolt.cc
    aircraft.cc
    kite.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::core rx::asset)
  # PRIVATE static archives, needed at final link. Wrapped in BUILD_INTERFACE so
  # they stay out of the installed export; rxConfig.cmake re-adds bundled
  # imported equivalents for find_package consumers.
  target_link_libraries(rx_physics PRIVATE
    $<BUILD_INTERFACE:Jolt> $<BUILD_INTERFACE:kinema::kinema>)
  set(RX_INSTALL_JOLT ON PARENT_SCOPE)
else()
  rx_add_module(physics
    physics_world_stub.cc
    vehicle_profiles.cc
    boat_profiles.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::core rx::asset)
endif()
