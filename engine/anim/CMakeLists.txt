rx_add_module(anim
  pose.cc
  locomotion.cc
  foot_ik.cc
)
target_link_libraries(rx_anim PUBLIC rx::core rx::asset)
