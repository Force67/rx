# rx::locomotion — physics-first articulated locomotion (docs/LOCOMOTION.md):
# a feedback controller that stands, walks and recovers a jointed ragdoll biped
# by measuring the simulation and driving joint motors toward continuous
# numeric targets. No animation clips or motion-block hierarchy; depends on
# rx::physics for bodies, joints, motors, contacts and casts.
rx_add_module(locomotion
  controller.cc
  estimator.cc
  footstep.cc
  gait.cc
  rig.cc
  whole_body.cc
)
target_link_libraries(rx_locomotion PUBLIC rx::core rx::physics)
