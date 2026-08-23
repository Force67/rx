# The rx viewer: an app::Application on top of app::Host, adding the built-in
# demo scenes / glTF loading, the fly camera and the imgui debug overlay. The
# debug overlay compiles to a stub when imgui is absent, so the same
# translation units serve headless builds.
set(RX_RUNTIME_SOURCES
  viewer.cc
  camera_input.cc
  viewer_input.cc
  demo_scenes.cc
  scene_authoring.cc
  scene_validate.cc
  material_palette.cc
  demo_grass.cc
  demo_nav.cc
  demo_placement.cc
  placement_demo_assets.cc
  demo_ship.cc
  demo_gym.cc
  demo_shooter.cc
  demo_puppet.cc
  demo_drive.cc
  feature_gym/feature_gym.cc
  scene_hook_demo.cc
  scene_hook_rhi_demo.cc
  fly_camera.cc
  showcase_camera.cc
  debug_ui.cc
)

set(RX_RUNTIME_MODULES
  rx::app
  rx::core
  rx::ecs
  rx::asset
  rx::scene
  # .rxscene text scenes: component reflection + the text loader.
  rx::edit
  # --authoring-endpoint / --dump-commands: the live script command surface.
  rx::authoring
  rx::script
  rx::audio
  rx::render
  rx::physics
  rx::anim
  rx::locomotion
  rx::net
  rx::net_viz
  rx::nav
  rx::nav_viz
  rx::weather
  rx::placement
  rx::placement_gpu
  rx::character
  rx::combat
  rx::inventory
  rx::inventory_world
)

# CI/release builds can provide a stable identifier with -DRX_BUILD_ID=... .
# Developer builds fall back to the source revision, or "source" outside Git.
set(RX_BUILD_ID "" CACHE STRING "Build identifier shown in the rx debug UI")
if(NOT RX_BUILD_ID)
  set(RX_GIT_REVISION_RESULT 1)
  find_package(Git QUIET)
  if(Git_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      RESULT_VARIABLE RX_GIT_REVISION_RESULT
      OUTPUT_VARIABLE RX_GIT_REVISION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    # Reconfigure when the checkout moves so a rebuilt binary cannot retain the
    # previous revision. HEAD covers detached checkouts; the ref covers branches.
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      OUTPUT_VARIABLE RX_GIT_HEAD_PATH
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      RESULT_VARIABLE RX_GIT_HEAD_REF_RESULT
      OUTPUT_VARIABLE RX_GIT_HEAD_REF
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(RX_GIT_HEAD_PATH)
      get_filename_component(RX_GIT_HEAD_PATH ${RX_GIT_HEAD_PATH} ABSOLUTE
        BASE_DIR ${PROJECT_SOURCE_DIR})
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RX_GIT_HEAD_PATH})
    endif()
    if(RX_GIT_HEAD_REF_RESULT EQUAL 0 AND RX_GIT_HEAD_REF)
      execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --git-path ${RX_GIT_HEAD_REF}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE RX_GIT_HEAD_REF_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
      get_filename_component(RX_GIT_HEAD_REF_PATH ${RX_GIT_HEAD_REF_PATH} ABSOLUTE
        BASE_DIR ${PROJECT_SOURCE_DIR})
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RX_GIT_HEAD_REF_PATH})
    endif()
  endif()
  if(RX_GIT_REVISION_RESULT EQUAL 0 AND RX_GIT_REVISION)
    set(RX_BUILD_ID ${RX_GIT_REVISION})
  else()
    set(RX_BUILD_ID source)
  endif()
endif()

add_executable(rx main.cc ${RX_RUNTIME_SOURCES})
target_link_libraries(rx PRIVATE ${RX_RUNTIME_MODULES})
# Radiance .hdr decode for imported UsdLux dome environment maps (viewer.cc).
target_link_libraries(rx PRIVATE rx::stb_impl)
target_compile_definitions(rx PRIVATE
  RX_BUILD_CONFIG="$<CONFIG>"
  RX_BUILD_ID="${RX_BUILD_ID}"
  RX_VERSION="${PROJECT_VERSION}")
rx_set_warnings(rx)
rx_enable_mimalloc(rx)

# `rx --validate`: no device and no window, so plain ctest runs it. The clean
# half matters more than the broken one - a check that fires on a scene we ship
# is worse than no check, because the next agent learns to ignore the report.
if(RX_BUILD_TESTS)
  # From the source root, because a scene names its assets (Model.path,
  # Surface.materialx) relative to the working directory.
  foreach(scene cornell showcase gallery material_sheet model)
    add_test(NAME scene_validate_${scene}
      COMMAND rx --validate ${CMAKE_CURRENT_SOURCE_DIR}/scenes/${scene}.rxscene
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endforeach()
  add_test(NAME scene_validate_broken
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken.rxscene --json
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_broken PROPERTIES WILL_FAIL TRUE)

  # Every material preset, one test each, globbed so a preset added later is
  # covered without touching this file. A preset is a fragment (no geometry of
  # its own, no camera, no light), which the validator recognizes: the
  # zero-warning assertion below is what keeps that recognition honest, because
  # a preset that reported "Surface with nothing to shade" would put a finding
  # on all 30 of them and teach the next author to skip the report.
  file(GLOB RX_MATERIAL_PRESETS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/scenes/materials/*.rxscene)
  foreach(preset ${RX_MATERIAL_PRESETS})
    get_filename_component(preset_name ${preset} NAME_WE)
    add_test(NAME material_preset_${preset_name}
      COMMAND rx --validate ${preset}
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(material_preset_${preset_name} PROPERTIES
      PASS_REGULAR_EXPRESSION "0 error\\(s\\), 0 warning\\(s\\)")
  endforeach()

  # ... and the other half of that check: a file that HAS a camera and a light
  # is a scene, so a Surface on an entity with no Shape in it is still the
  # author's bug. Without this the fragment rule could silently grow to cover
  # every file and the check would be gone rather than fixed.
  add_test(NAME scene_validate_surface_without_shape
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken.rxscene --json
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_surface_without_shape PROPERTIES
    PASS_REGULAR_EXPRESSION "surface_without_shape.*Material with nothing to be")

  # discarded_transform, both halves at once. The COUNT is the assertion: the
  # fixture contradicts the placing pass three times and agrees with it twice,
  # and the two that agree are what a SaveScene round trip writes, so a check
  # that reported them would fire on every saved scene. Four warnings means the
  # check stopped distinguishing the two.
  add_test(NAME scene_validate_discarded_transform
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/discarded_transform.rxscene
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_discarded_transform PROPERTIES
    PASS_REGULAR_EXPRESSION "0 error\\(s\\), 3 warning\\(s\\)")

  # Stretch's own failure mode, which the exit code cannot tell from the two
  # dozen other findings in that fixture: a non-positive axis is what the normal
  # bake would divide by, so it is an error rather than a warning about an
  # odd-looking number.
  add_test(NAME scene_validate_degenerate_stretch
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken.rxscene --json
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_degenerate_stretch PROPERTIES
    PASS_REGULAR_EXPRESSION "degenerate_stretch.*Squashed to nothing.*Stretch.scale y is 0")

  # `--dump-materials`: the palette listing an author picks a name from. It
  # loads every preset, so this also proves the whole palette parses; the regex
  # is the shape a caller reads (name -> the path it writes into Prefab.path).
  add_test(NAME dump_materials
    COMMAND rx --dump-materials
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(dump_materials PROPERTIES
    PASS_REGULAR_EXPRESSION
      "\"name\": \"brushed_aluminium\", \"path\": \"runtime/scenes/materials/brushed_aluminium.rxscene\"")

  add_test(NAME dump_materials_missing_directory
    COMMAND rx --dump-materials ${PROJECT_SOURCE_DIR}/test/data/no_such_palette
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(dump_materials_missing_directory PROPERTIES WILL_FAIL TRUE)
  # The two halves of the glTF reference check, which a pass/fail exit code
  # alone cannot tell apart: scene_validate_model above proves references that
  # resolve stay quiet, and this proves the one naming a mesh a real file does
  # not have is the thing that fires. PASS_REGULAR_EXPRESSION ignores the exit
  # code, which is nonzero here for every other finding in that fixture.
  add_test(NAME scene_validate_model_reference
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken.rxscene --json
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_model_reference PROPERTIES
    PASS_REGULAR_EXPRESSION "unresolved_mesh.*TwoBoxes.gltf#mesh9.*a file that has 2")

  # The layout failures. One fixture per anchor failure, because they are found
  # in two different phases: a dangling target and a cycle are both settled
  # while ordering the anchors, so a file carrying either never reaches the
  # placement that a target with nothing to measure fails at. The grid and
  # prefab failures are three separate passes and share one file.
  foreach(case broken_layout anchor_cycle anchor_no_bounds prefab_cycle)
    add_test(NAME scene_validate_${case}
      COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/${case}.rxscene
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(scene_validate_${case} PROPERTIES WILL_FAIL TRUE)
  endforeach()

  # ... and which check fired, which the exit code above cannot tell apart. The
  # regexes name the offending line as well as the message, because a finding
  # that lands on the wrong entity's block is the failure mode these messages
  # exist to avoid.
  add_test(NAME scene_validate_grid_overflow
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken_layout.rxscene
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_grid_overflow PROPERTIES
    PASS_REGULAR_EXPRESSION "broken_layout.rxscene:28.*Grid.of = \"Row\" names a grid of 1 cells")

  add_test(NAME scene_validate_prefab_missing
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken_layout.rxscene
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_prefab_missing PROPERTIES
    PASS_REGULAR_EXPRESSION "Prefab.path = \"no_such_prefab.rxscene\" does not load")

  add_test(NAME scene_validate_anchor_dangling
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/broken_layout.rxscene
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_anchor_dangling PROPERTIES
    PASS_REGULAR_EXPRESSION "broken_layout.rxscene:41.*names no entity in this scene")

  add_test(NAME scene_validate_anchor_cycle_names_loop
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/anchor_cycle.rxscene
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_anchor_cycle_names_loop PROPERTIES
    PASS_REGULAR_EXPRESSION "closes a cycle \\(Lower -> Upper -> Lower\\)")

  # The guard that keeps a self-instancing prefab from expanding until memory
  # runs out, and the message that says which file closed the loop.
  add_test(NAME scene_validate_prefab_cycle_names_loop
    COMMAND rx --validate ${PROJECT_SOURCE_DIR}/test/data/prefab_cycle.rxscene
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(scene_validate_prefab_cycle_names_loop PROPERTIES
    PASS_REGULAR_EXPRESSION "prefab_cycle_inner.rxscene:13.*instances itself")
endif()

file(GLOB RX_FEATURE_GYM_ASSETS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/feature_gym/assets/*)
set_property(TARGET rx APPEND PROPERTY LINK_DEPENDS ${RX_FEATURE_GYM_ASSETS})
add_custom_command(TARGET rx POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/feature_gym/assets
    $<TARGET_FILE_DIR:rx>/feature_gym-assets
  COMMENT "Copying feature-gym assets")
add_custom_target(rx-feature-gym-assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/feature_gym/assets
    $<TARGET_FILE_DIR:rx>/feature_gym-assets
  DEPENDS ${RX_FEATURE_GYM_ASSETS}
  COMMENT "Refreshing feature-gym assets")

# --demo scenehook: the app builds its own raw-Vulkan pipeline through the render
# interop, so its shaders embed here instead of in the render module. They read a
# buffer-device-address arena (RawBufferLoad/Store), a SPIR-V-only construct, so
# they cannot target DXIL (no d3d12 sidecar).
set(RX_SHADER_NO_DXIL
  scenehook_cull.cs.hlsl
  scenehook.vs.hlsl
  scenehook.ps.hlsl
  scenehook_rhi_cull.cs.hlsl
  scenehook_rhi.vs.hlsl
  scenehook_rhi.ps.hlsl
  scenehook_rhi.ms.hlsl
)
rx_embed_shaders(rx
  shaders/scenehook_cull.cs.hlsl
  shaders/scenehook.vs.hlsl
  shaders/scenehook.ps.hlsl
  shaders/scenehook_rhi_cull.cs.hlsl
  shaders/scenehook_rhi.vs.hlsl
  shaders/scenehook_rhi.ps.hlsl
  shaders/scenehook_rhi.ms.hlsl
)

if(TARGET rx_imgui)
  target_link_libraries(rx PRIVATE rx::imgui)
  target_compile_definitions(rx PRIVATE RX_HAS_IMGUI=1)
  # Default location of the editable .ini render presets (engine/render/presets).
  # The debug ui lists and loads them; override with the env var RX_PRESETS_DIR.
  # Absolute so the dev build finds the source files.
  target_compile_definitions(rx PRIVATE
    RX_PRESETS_DIR_DEFAULT="${PROJECT_SOURCE_DIR}/engine/render/presets")
endif()
