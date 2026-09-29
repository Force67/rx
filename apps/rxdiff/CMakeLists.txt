add_executable(rxdiff main.cc)
target_link_libraries(rxdiff PRIVATE rx::stb_impl equilibrium::base)
rx_set_warnings(rxdiff)
