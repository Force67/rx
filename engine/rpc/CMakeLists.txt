rx_add_module(rpc
  rpc_value.cc
  rpc_message.cc
  rpc_registry.cc
)

# A typed value, a wire codec, and a name-to-handler registry for remote
# procedure calls. Pure logic with no transport dependency: the net layer carries
# the encoded calls, the managed runtime registers handlers.
target_link_libraries(rx_rpc PUBLIC rx::core)
