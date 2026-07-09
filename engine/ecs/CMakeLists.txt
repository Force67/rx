rx_add_module(ecs
  archetype.cc
  world.cc
  scheduler.cc
)

target_link_libraries(rx_ecs PUBLIC rx::core)
