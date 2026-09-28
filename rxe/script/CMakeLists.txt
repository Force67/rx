rx_add_module(script
  script_string.cc
  script_value.cc
  script_arena.cc
  script_symbols.cc
  handler_registry.cc
)

# The runtime-agnostic script substrate: a portable value/string vocabulary
# (ScriptString/ScriptValue), a symbol interner, a per-call arena, and a
# name->handler registry. It knows about NO game system; each system (scene,
# audio, ...) contributes its own handlers in its own module and calls itself
# directly, so this stays a low-level dependency of them, not the reverse. Only
# core + ecs (the shared component backbone the HandlerContext points at).
target_link_libraries(rx_script PUBLIC rx::foundation rx::ecs)

if(RX_BUILD_TESTS)
  # rx::script unit tests: the portable value/string primitives and the handler
  # registry driven end-to-end against a real ecs::World, with no runtime, GPU or
  # transport, so plain `ctest` runs them everywhere.
  add_executable(script_test script_test.cc)
  target_link_libraries(script_test PRIVATE rx::scene rx::script rx::ecs rx::foundation)
  add_test(NAME script_test COMMAND script_test)
endif()
