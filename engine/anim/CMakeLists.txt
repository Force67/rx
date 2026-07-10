rx_add_module(anim
  pose.cc
  locomotion.cc
  foot_ik.cc
  anim_graph.cc
  rig_player.cc
  foot_placement.cc
)
target_link_libraries(rx_anim PUBLIC rx::core rx::asset)
# kinema is a PRIVATE implementation detail of the graph/rig/foot-placement units
# (it never appears in a public anim header, mirroring how physics consumes it).
# BUILD_INTERFACE keeps it out of the installed export; rxConfig re-attaches the
# bundled imported archive for find_package consumers.
target_link_libraries(rx_anim PRIVATE $<BUILD_INTERFACE:kinema::kinema>)
