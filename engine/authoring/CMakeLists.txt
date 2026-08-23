rx_add_module(authoring
  command_bridge.cc
  command_endpoint.cc
)

# The live authoring endpoint: the script command registry exposed over the rpc
# wire vocabulary on a local unix socket, so an out-of-process tool can drive a
# RUNNING engine. It owns the trust boundary (see command_bridge.h), which is why
# it is a module of its own rather than a dependency added to either side: script
# stays runtime-agnostic and rpc stays transport-free.
target_link_libraries(rx_authoring PUBLIC rx::core rx::ecs rx::rpc rx::script)
