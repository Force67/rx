add_library(rx_scene INTERFACE)
add_library(rx::scene ALIAS rx_scene)
target_include_directories(rx_scene INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(rx_scene INTERFACE rx::core rx::asset)
