rx_add_module(terrain
  terrain.cc
  terrain_io.cc
)

target_link_libraries(rx_terrain PUBLIC rx::foundation rx::asset rx::scene)

if(RX_BUILD_TESTS)
  add_executable(terrain_test terrain_test.cc)
  target_link_libraries(terrain_test PRIVATE rx::terrain rx::asset rx::scene rx::foundation)
  add_test(NAME terrain_test COMMAND terrain_test)
endif()
