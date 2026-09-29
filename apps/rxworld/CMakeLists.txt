add_executable(rxworld main.cc)
target_link_libraries(rxworld PRIVATE rx::world rx::asset rx::ecs rx::foundation)
rx_set_warnings(rxworld)
