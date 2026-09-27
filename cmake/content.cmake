# The install layout an rx executable runs against (docs/CONFIG.md): rxe/config/
# and Data/ next to the binary. rx_stage_content(<target>) reproduces it beside
# a build-tree executable, re-copied on every build so an edited ini needs no
# relink:
#
#   <target dir>/rxe/config/     <- rx's config/ (the platform tiers)
#   <target dir>/Data/*.rxp      <- the engine archives
#
# Executables that share a directory are staged together, in one call
# (rx_stage_content(game tool)), or two copies race into the same files. A game
# stages its own config/ and archives the same way.
set(RX_CONTENT_CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/../config CACHE INTERNAL "")
set(RX_CONTENT_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/Data CACHE INTERNAL "")

function(rx_stage_content target)
  set(dir $<TARGET_FILE_DIR:${target}>)
  add_custom_target(${target}_rx_content ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${RX_CONTENT_CONFIG_DIR} ${dir}/rxe/config
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RX_CONTENT_DATA_DIR} ${dir}/Data
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${RX_CONTENT_DATA_DIR} ${dir}/Data
    COMMENT "Staging rx content beside ${target}"
    VERBATIM)
  # The archives' target is declared after rx's own executables; those pick up
  # the dependency from rx_finish_content_staging.
  if(TARGET rx_engine_archives)
    add_dependencies(${target}_rx_content rx_engine_archives)
  else()
    set_property(GLOBAL APPEND PROPERTY RX_CONTENT_STAGING_TARGETS ${target}_rx_content)
  endif()
  foreach(t ${target} ${ARGN})
    add_dependencies(${t} ${target}_rx_content)
  endforeach()
endfunction()

function(rx_finish_content_staging)
  get_property(targets GLOBAL PROPERTY RX_CONTENT_STAGING_TARGETS)
  foreach(t ${targets})
    add_dependencies(${t} rx_engine_archives)
  endforeach()
endfunction()
