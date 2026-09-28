rx_add_module(ecs
  archetype.cc
  world.cc
  scheduler.cc
)

target_link_libraries(rx_ecs PUBLIC rx::foundation)

if(RX_BUILD_TESTS)
  # Chunked ECS storage: spawn storms, non-POD relocation, chunk reclamation.
  add_executable(ecs_test ecs_test.cc)
  target_link_libraries(ecs_test PRIVATE rx::ecs rx::foundation)
  rx_enable_mimalloc(ecs_test)
  add_test(NAME ecs_test COMMAND ecs_test)
endif()
