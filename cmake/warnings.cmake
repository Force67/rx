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
  # BUILD_INTERFACE keeps in-tree (add_subdirectory) consumers seeing the source
  # header root unchanged; INSTALL_INTERFACE points find_package() consumers at
  # the installed, module-qualified include root (see cmake/install.cmake).
  target_include_directories(rx_${name} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/engine>
    $<INSTALL_INTERFACE:include>)
  # C++23 is a hard usage requirement (base:: headers use deducing this etc.);
  # propagate it so installed consumers compile with the right standard.
  target_compile_features(rx_${name} PUBLIC cxx_std_23)
  rx_set_warnings(rx_${name})
endfunction()
