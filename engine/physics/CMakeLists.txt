if(RX_JOLT)
  rx_add_module(physics
    physics_world.cc
    kinema_jolt.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::core rx::asset)
  target_link_libraries(rx_physics PRIVATE Jolt kinema::kinema)
else()
  rx_add_module(physics
    physics_world_stub.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::core rx::asset)
endif()
