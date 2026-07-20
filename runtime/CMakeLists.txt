# The rx viewer: an app::Application on top of app::Host, adding the built-in
# demo scenes / glTF loading, the fly camera and the imgui debug overlay. The
# debug overlay compiles to a stub when imgui is absent, so the same
# translation units serve headless builds.
set(RX_RUNTIME_SOURCES
  viewer.cc
  camera_input.cc
  viewer_input.cc
  demo_scenes.cc
  demo_grass.cc
  demo_nav.cc
  demo_placement.cc
  placement_demo_assets.cc
  demo_ship.cc
  demo_gym.cc
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
target_compile_definitions(rx PRIVATE
  RX_BUILD_CONFIG="$<CONFIG>"
  RX_BUILD_ID="${RX_BUILD_ID}"
  RX_VERSION="${PROJECT_VERSION}")
rx_set_warnings(rx)
rx_enable_mimalloc(rx)

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
