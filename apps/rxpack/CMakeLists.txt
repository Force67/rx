add_executable(rxpack main.cc)
target_link_libraries(rxpack PRIVATE rx::asset rx::foundation)
rx_set_warnings(rxpack)
