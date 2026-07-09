function(rx_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    # missing-field-initializers fights designated init of vulkan structs
    # where value initializing the rest is exactly the point.
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter
      -Wno-missing-field-initializers)
  endif()
endfunction()

function(rx_add_module name)
  add_library(rx_${name} STATIC ${ARGN})
  add_library(rx::${name} ALIAS rx_${name})
  target_include_directories(rx_${name} PUBLIC ${CMAKE_SOURCE_DIR}/engine)
  rx_set_warnings(rx_${name})
endfunction()
