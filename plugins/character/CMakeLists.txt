# rx::character: ECS-driven player-character support, a capsule locomotion
# controller (walk/run/sprint/crouch/jump over Jolt CharacterVirtual) plus
# first/third-person view modes composed from the scene camera rig. Systems are
# free functions; components are plain data. Depends on rx::physics for the
# character controller and swept queries, and rx::scene for the camera rig it
# drives; no game-specific policy lives here.
rx_add_module(character
  character.cc
  jetpack.cc
)
target_link_libraries(rx_character PUBLIC rx::foundation rx::ecs rx::scene rx::physics)

if(RX_BUILD_TESTS AND RX_JOLT)
  # rx::character locomotion + view modes over the real Jolt character controller:
  # gait acceleration, crouch capsule/eye blend, ceiling-blocked uncrouch, jump +
  # land, step-up vs blocked, third-person camera obstruction pull-in and the
  # FP<->TP view-mode stack transition. Headless (no GPU), needs Jolt.
  add_executable(character_test character_test.cc)
  target_link_libraries(character_test PRIVATE rx::character rx::physics rx::scene rx::ecs rx::foundation)
  add_test(NAME character_test COMMAND character_test)

  # rx::character jetpack over the character controller's velocity seam: full-burn
  # climb, fuel burn to empty + dead-stick fall, grounded-only refuel, spool lag to
  # 90%, airborne lateral thrust-vector authority and NaN-free / jump-still-works.
  # Headless (no GPU), needs Jolt.
  add_executable(jetpack_test jetpack_test.cc)
  target_link_libraries(jetpack_test PRIVATE rx::character rx::physics rx::scene rx::ecs rx::foundation)
  add_test(NAME jetpack_test COMMAND jetpack_test)
endif()
