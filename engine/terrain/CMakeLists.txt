rx_add_module(terrain
  terrain.cc
  terrain_io.cc
)

target_link_libraries(rx_terrain PUBLIC rx::core rx::asset rx::scene)
