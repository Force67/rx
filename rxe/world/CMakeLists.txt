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
target_link_libraries(rx_world PUBLIC rx::foundation rx::ecs rx::asset rx::scene rx::edit)

if(RX_BUILD_TESTS)
  add_executable(world_format_test world_format_test.cc)
  target_link_libraries(world_format_test PRIVATE rx::world rx::foundation)
  add_test(NAME world_format_test COMMAND world_format_test)

  add_executable(world_map_test world_map_test.cc)
  target_link_libraries(world_map_test PRIVATE rx::world rx::asset rx::scene rx::foundation)
  add_test(NAME world_map_test COMMAND world_map_test)

  add_executable(world_stream_test world_stream_test.cc)
  target_link_libraries(world_stream_test PRIVATE rx::world rx::edit rx::asset rx::scene rx::ecs rx::foundation)
  add_test(NAME world_stream_test COMMAND world_stream_test)

  add_executable(world_overlay_test world_overlay_test.cc)
  target_link_libraries(world_overlay_test PRIVATE rx::world rx::foundation)
  add_test(NAME world_overlay_test COMMAND world_overlay_test)

  # Random mutation over the three decoders. The hand-written refusal tests
  # each aim one mutation at one check; this aims many at none in particular,
  # which is the half they cannot cover. Cheap enough to keep in the default
  # suite; run it under -DRX_SANITIZE=ON for the memory-safety half to bite,
  # and pass an iteration count to run it longer than the suite does.
  add_executable(world_fuzz_test world_fuzz_test.cc)
  target_link_libraries(world_fuzz_test PRIVATE rx::world rx::foundation)
  add_test(NAME world_fuzz_test COMMAND world_fuzz_test)

  # The cook's per-entity verdict, which the editor shows for the selection.
  # A label that lies about what an object will become is worse than no label.
  add_executable(world_classify_test world_classify_test.cc)
  target_link_libraries(world_classify_test PRIVATE rx::world rx::edit rx::ecs rx::foundation)
  add_test(NAME world_classify_test COMMAND world_classify_test)
endif()

if(RX_BUILD_TESTS AND RX_BUILD_TOOLS)
  # The whole path in one test: an authored scene through the real baker into
  # a real archive, mounted and streamed back. Needs the tool, so it is only
  # registered when the tools are built.
  add_executable(world_bake_test world_bake_test.cc)
  target_link_libraries(world_bake_test PRIVATE rx::world rx::asset rx::scene rx::ecs rx::foundation)
  add_test(NAME world_bake_test COMMAND world_bake_test $<TARGET_FILE:rxworld>)
endif()
