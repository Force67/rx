# rx::combat: the shooter half of a first-person game (weapon definitions,
# trigger/fire-rate/burst discipline, spread and bloom, view recoil, reloading,
# aim-down-sights, hitscan resolve with falloff/penetration/impulse, ballistic
# projectiles, explosions and health/armor/teams). Systems are free functions;
# components are plain data. Depends on rx::physics for the queries a shot
# resolves through, and rx::scene for the Transform a projectile carries. No
# game policy here.
rx_add_module(combat
  weapon_def.cc
  damage.cc
  weapon.cc
  projectile.cc
)
target_link_libraries(rx_combat PUBLIC rx::foundation rx::ecs rx::scene rx::physics)

if(RX_BUILD_TESTS AND RX_JOLT)
  # rx::combat over real Jolt: fire rate and modes, magazine/reserve reloading,
  # spread bloom, view recoil kick + recovery, hitscan falloff/zone/penetration,
  # ballistic projectiles, explosion falloff and line of sight. Headless.
  add_executable(combat_test combat_test.cc)
  target_link_libraries(combat_test PRIVATE rx::combat rx::physics rx::scene rx::ecs rx::foundation)
  add_test(NAME combat_test COMMAND combat_test)
endif()
