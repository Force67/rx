rx_add_module(script
  script_string.cc
  script_value.cc
  script_arena.cc
  script_symbols.cc
  handler_registry.cc
)

# The runtime-agnostic script substrate: a portable value/string vocabulary
# (ScriptString/ScriptValue), a symbol interner, a per-call arena, and a
# name->handler registry. It knows about NO game system -- each system (scene,
# audio, ...) contributes its own handlers in its own module and calls itself
# directly, so this stays a low-level dependency of them, not the reverse. Only
# core + ecs (the shared component backbone the HandlerContext points at).
target_link_libraries(rx_script PUBLIC rx::core rx::ecs)
