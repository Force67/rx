# The baked map: cooked world index + cell payloads, and the runtime that
# streams them out of a mounted archive into the ECS. Depends on the data
# modules only (edit supplies the component reflection a cooked column is
# named and validated by); rx::scene owns the streaming planner and must never
# name rx::world back.
rx_add_module(world
  world_format.cc
  world_map.cc
  world_bake.cc
  world_claim.cc
  world_overlay.cc
  world_stream.cc
)
target_link_libraries(rx_world PUBLIC rx::core rx::ecs rx::asset rx::scene rx::edit)
