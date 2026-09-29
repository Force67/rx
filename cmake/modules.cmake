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
  # CMakeLists). See foundation/build_config/export.h for the annotation scheme.
  add_library(rx_${name} ${RX_LIB_TYPE} ${ARGN})
  add_library(rx::${name} ALIAS rx_${name})
  # Includes are spelled from the repository root ("rxe/physics/...",
  # "plugins/nav/...", "foundation/..."). BUILD_INTERFACE points in-tree
  # (add_subdirectory) consumers at the source root; INSTALL_INTERFACE points
  # find_package() consumers at the installed include root, which mirrors it
  # (see cmake/install.cmake).
  target_include_directories(rx_${name} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>)
  # C++23 is a hard usage requirement (base:: headers use deducing this etc.);
  # propagate it so installed consumers compile with the right standard.
  target_compile_features(rx_${name} PUBLIC cxx_std_23)

  # Hidden visibility ALWAYS (both static and shared builds) so the export
  # annotation set is honest: an internal symbol another module reaches without
  # an export macro fails to link in the shared build instead of silently
  # working. RX_SHARED_BUILD (public) turns the export macros on for everyone
  # who sees the headers; in the static build they expand to nothing.
  set_target_properties(rx_${name} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  # RX_<MODULE>_EXPORT comes from here, not from a shared header, so a module
  # (a game's or a third-party plugin's included) needs no line in any file rx
  # owns: its own sources see the export side, everyone linking it the import
  # side. Both resolve through foundation/build_config/export.h's RX_DSO_*.
  string(TOUPPER ${name} name_uc)
  target_compile_definitions(rx_${name}
    PRIVATE RX_${name_uc}_IMPLEMENTATION "RX_${name_uc}_EXPORT=RX_DSO_EXPORT"
    INTERFACE "RX_${name_uc}_EXPORT=RX_DSO_IMPORT")
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
