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
target_link_libraries(rx_combat PUBLIC rx::core rx::ecs rx::scene rx::physics)
