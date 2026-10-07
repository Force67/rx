# librxc: rx behind a C ABI, one shared object that exports only rxc_*.
# Every rx module links in statically (RX_BUILD_RXC turns PIC on for them).
add_library(rxc SHARED rx_c.cc)
target_link_libraries(rxc PRIVATE rx::app)
if(TARGET rx_imgui AND TARGET rx_imgui_renderer)
  target_link_libraries(rxc PRIVATE rx::imgui rx::imgui_renderer)
else()
  message(FATAL_ERROR "rxc draws its hud through imgui, which needs SDL3")
endif()
set_target_properties(rxc PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
if(NOT WIN32 AND NOT APPLE)
  # Keep the statically linked engine out of the dynamic symbol table, so a
  # host program only ever binds rxc_* and never collides with rx internals.
  target_link_options(rxc PRIVATE -Wl,--exclude-libs,ALL -Wl,--no-undefined)
endif()
rx_set_warnings(rxc)
