# The install layout an rx executable runs against (docs/CONFIG.md), reproduced
# beside build-tree executables and re-copied on every build:
#
#   <target dir>/Data/rx_engine.rxp   everything the engine ships (fonts, tiers)
#   <target dir>/rxe/config/          the tiers loose, LOOSE_ENGINE_CONFIG only
#
# LOOSE_ENGINE_CONFIG is for engine builds (the viewer, the editor), where the
# tiers are edited; a game ships only the archive. Executables that share a
# directory are staged together, in one call (rx_stage_content(game tool)), or
# two copies race into the same files. A game stages its own config/ and
# archives the same way.
set(RX_CONTENT_CONFIG_DIR ${CMAKE_CURRENT_LIST_DIR}/../config CACHE INTERNAL "")
set(RX_CONTENT_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/Data CACHE INTERNAL "")

function(rx_stage_content)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "LOOSE_ENGINE_CONFIG" "" "")
  list(GET ARG_UNPARSED_ARGUMENTS 0 target)
  set(dir $<TARGET_FILE_DIR:${target}>)
  set(loose)
  if(ARG_LOOSE_ENGINE_CONFIG)
    set(loose COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                      ${RX_CONTENT_CONFIG_DIR} ${dir}/rxe/config)
  endif()
  add_custom_target(${target}_rx_content ALL
    ${loose}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RX_CONTENT_DATA_DIR} ${dir}/Data
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${RX_CONTENT_DATA_DIR} ${dir}/Data
    COMMENT "Staging rx content beside ${target}"
    VERBATIM)
  # The archive's target is declared after rx's own executables; those pick up
  # the dependency from rx_finish_content_staging.
  if(TARGET rx_engine_archives)
    add_dependencies(${target}_rx_content rx_engine_archives)
  else()
    set_property(GLOBAL APPEND PROPERTY RX_CONTENT_STAGING_TARGETS ${target}_rx_content)
  endif()
  foreach(t ${ARG_UNPARSED_ARGUMENTS})
    add_dependencies(${t} ${target}_rx_content)
  endforeach()
endfunction()

function(rx_finish_content_staging)
  get_property(targets GLOBAL PROPERTY RX_CONTENT_STAGING_TARGETS)
  foreach(t ${targets})
    add_dependencies(${t} rx_engine_archives)
  endforeach()
endfunction()
