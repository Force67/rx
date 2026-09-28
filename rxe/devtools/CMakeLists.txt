rx_add_module(devtools
  command_bridge.cc
  command_endpoint.cc
)

# The live authoring endpoint: the script command registry exposed over the rpc
# wire vocabulary on a local unix socket, so an out-of-process tool can drive a
# RUNNING engine. It owns the trust boundary (see command_bridge.h), which is why
# it is a module of its own rather than a dependency added to either side: script
# stays runtime-agnostic and rpc stays transport-free.
target_link_libraries(rx_devtools PUBLIC rx::foundation rx::ecs rx::rpc rx::script)

if(RX_BUILD_TESTS AND NOT WIN32)
  # rx::devtools unit tests: the rpc <-> script marshalling, the trust gate, and
  # the unix socket endpoint driven with a real client fd. No GPU or window, but
  # unix sockets, so it stays off Windows like the endpoint does.
  add_executable(command_bridge_test command_bridge_test.cc)
  target_link_libraries(command_bridge_test PRIVATE rx::devtools rx::scene rx::script
    rx::rpc rx::ecs rx::foundation)
  add_test(NAME command_bridge_test COMMAND command_bridge_test)
endif()
