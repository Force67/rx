# rx::events: raw input state, named actions and axes, and the bindings that map
# one onto the other. No window: the window module fills InputState.
rx_add_module(events input_bindings.cc)
target_link_libraries(rx_events PUBLIC rx::foundation)

if(RX_BUILD_TESTS)
  # Touch contact state machine: header-only logic over input.h, so it needs
  # no window or panel and covers the lifecycle rules a touchscreen would
  # otherwise be the only way to exercise.
  add_executable(input_touch_test input_touch_test.cc)
  target_link_libraries(input_touch_test PRIVATE rx::events rx::foundation)
  add_test(NAME input_touch_test COMMAND input_touch_test)

  # Action edges from raw input, including presses that begin and end inside
  # one pump.
  add_executable(input_map_test input_map_test.cc)
  target_link_libraries(input_map_test PRIVATE rx::events rx::foundation)
  add_test(NAME input_map_test COMMAND input_map_test)
endif()
