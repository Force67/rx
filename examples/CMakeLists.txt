add_executable(body_jiggle_example body_jiggle.cc)
target_link_libraries(body_jiggle_example PRIVATE rx::anim rx::asset rx::core)
rx_set_warnings(body_jiggle_example)
