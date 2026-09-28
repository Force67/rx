if(RX_JOLT)
  rx_add_module(physics
    cloth_collision.cc
    physics_world.cc
    kinema_jolt.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::foundation rx::asset)
  # PRIVATE static archives, needed at final link. Wrapped in BUILD_INTERFACE so
  # they stay out of the installed export; rxConfig.cmake re-adds bundled
  # imported equivalents for find_package consumers.
  target_link_libraries(rx_physics PRIVATE
    $<BUILD_INTERFACE:Jolt> $<BUILD_INTERFACE:kinema::kinema>)
  set(RX_INSTALL_JOLT ON PARENT_SCOPE)
else()
  # Without Jolt the world is a stub whose bodies are all invalid, so every
  # consumer (rx::vehicles, demo_drive.cc) still links and degrades to inert.
  rx_add_module(physics
    physics_world_stub.cc
    cloth_collision.cc
  )
  target_link_libraries(rx_physics PUBLIC rx::foundation rx::asset)
endif()

if(RX_BUILD_TESTS AND RX_JOLT)
  # Vehicle realism over the real Jolt vehicle path: acceleration + auto-shift,
  # surface-dependent braking (ice vs asphalt), rain wetness, manual gearbox,
  # telemetry ranges and water aquaplaning. Headless (no GPU), needs Jolt.
  add_executable(vehicle_test vehicle_test.cc)
  target_link_libraries(vehicle_test PRIVATE rx::physics rx::foundation)
  add_test(NAME vehicle_test COMMAND vehicle_test)
endif()

if(RX_BUILD_TESTS)
  add_executable(cloth_collision_test cloth_collision_test.cc)
  target_link_libraries(cloth_collision_test PRIVATE rx::physics rx::foundation)
  add_test(NAME cloth_collision_test COMMAND cloth_collision_test)
endif()

if(RX_BUILD_TESTS AND RX_JOLT)
  # Strand-hair simulation: builds the braid + ponytail test grooms, steps the
  # Jolt soft-body strand sim under a static and a moving head transform, and
  # asserts pins, cross-strand binds and head collision hold. Headless, no GPU.
  add_executable(hair_sim_test hair_sim_test.cc)
  target_link_libraries(hair_sim_test PRIVATE rx::render rx::physics rx::foundation)
  add_test(NAME hair_sim_test COMMAND hair_sim_test)

  add_executable(cloth_sim_test cloth_sim_test.cc)
  target_link_libraries(cloth_sim_test PRIVATE rx::physics rx::foundation)
  add_test(NAME cloth_sim_test COMMAND cloth_sim_test)
  add_test(NAME cloth_feature_off_test COMMAND cloth_sim_test --feature-off)
  set_tests_properties(cloth_feature_off_test PROPERTIES ENVIRONMENT "RX_FEATURES=-physics.cloth")

  # rx::physics body/joint adapter surface the locomotion controller relies on:
  # velocity/mass/COM readback, force + torque application, watched contacts,
  # motor torque limits. Headless, needs Jolt.
  add_executable(physics_body_test physics_body_test.cc)
  target_link_libraries(physics_body_test PRIVATE rx::physics rx::foundation)
  add_test(NAME physics_body_test COMMAND physics_body_test)
endif()
