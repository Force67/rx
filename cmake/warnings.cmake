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
  # STATIC by default; SHARED under -DRX_SHARED=ON (RX_LIB_TYPE set in the top
  # CMakeLists). See engine/core/export.h for the annotation scheme.
  add_library(rx_${name} ${RX_LIB_TYPE} ${ARGN})
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

  # Hidden visibility ALWAYS (both static and shared builds) so the export
  # annotation set is honest: an internal symbol another module reaches without
  # an export macro fails to link in the shared build instead of silently
  # working. RX_<MODULE>_IMPLEMENTATION (private) selects the export side of the
  # per-module macro; RX_SHARED_BUILD (public) turns the macros on for everyone
  # who sees the headers. In the static build both are effectively no-ops.
  set_target_properties(rx_${name} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  string(TOUPPER ${name} name_uc)
  target_compile_definitions(rx_${name} PRIVATE RX_${name_uc}_IMPLEMENTATION)
  if(RX_SHARED)
    target_compile_definitions(rx_${name} PUBLIC RX_SHARED_BUILD)
    # Fail the build if a module .so references a cross-DSO symbol its linked
    # libraries do not provide, instead of deferring it to a runtime load error.
    # This is what makes the DLL test complete: the annotation worklist is the
    # set of link errors, not a game of whack-a-mole at startup.
    if(NOT MSVC AND NOT APPLE)
      target_link_options(rx_${name} PRIVATE LINKER:--no-undefined)
    endif()
  endif()

  rx_set_warnings(rx_${name})
endfunction()
