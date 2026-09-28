# rx::events: raw input state, named actions and axes, and the bindings that map
# one onto the other. No window: the window module fills InputState.
rx_add_module(events input_bindings.cc)
target_link_libraries(rx_events PUBLIC rx::foundation)
