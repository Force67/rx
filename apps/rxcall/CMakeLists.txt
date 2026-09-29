add_executable(rxcall main.cc)
target_link_libraries(rxcall PRIVATE rx::rpc rx::foundation)
rx_set_warnings(rxcall)
