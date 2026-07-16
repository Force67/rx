# rx::character — ECS-driven player-character support: a capsule locomotion
# controller (walk/run/sprint/crouch/jump over Jolt CharacterVirtual) plus
# first/third-person view modes composed from the scene camera rig. Systems are
# free functions; components are plain data. Depends on rx::physics for the
# character controller and swept queries, and rx::scene for the camera rig it
# drives — no game-specific policy lives here.
rx_add_module(character
  character.cc
)
target_link_libraries(rx_character PUBLIC rx::core rx::ecs rx::scene rx::physics)
