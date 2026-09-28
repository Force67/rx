# rx::locomotion: physics-first articulated locomotion (docs/LOCOMOTION.md):
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
target_link_libraries(rx_locomotion PUBLIC rx::foundation rx::physics)

if(RX_BUILD_TESTS)
  # rx::locomotion pure math (gait clock, capture point, step clamping, swing
  # trajectory, leg IK): no Jolt, no GPU, plain ctest everywhere.
  add_executable(locomotion_math_test locomotion_math_test.cc)
  target_link_libraries(locomotion_math_test PRIVATE rx::locomotion rx::foundation)
  add_test(NAME locomotion_math_test COMMAND locomotion_math_test)
endif()

if(RX_BUILD_TESTS AND RX_JOLT)
  # rx::locomotion over real Jolt: rig build sanity, joint motor target
  # convergence, standing survival, push recovery, walking speed tracking and
  # controlled falls; the acceptance suite of docs/LOCOMOTION.md. Headless.
  add_executable(locomotion_test locomotion_test.cc)
  target_link_libraries(locomotion_test PRIVATE rx::locomotion rx::physics rx::foundation)
  add_test(NAME locomotion_test COMMAND locomotion_test)
endif()
