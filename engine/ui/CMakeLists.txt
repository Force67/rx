# The engine's retained-mode UI layer: libultragui driven in draw-data mode by
# rx's own Vulkan backend, plus the splash plate every rx application shows at
# startup. rx::app links this, so the plate needs no cooperation from the game;
# the editor links it for its whole UI.

# libultragui: sibling checkout ../libultragui (discovered like ../kinema).
if(NOT TARGET ultragui)
  set(RX_LIBULTRAGUI_DIR "${PROJECT_SOURCE_DIR}/../libultragui"
      CACHE PATH "Path to a libultragui checkout")
  if(NOT EXISTS "${RX_LIBULTRAGUI_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "libultragui not found at ${RX_LIBULTRAGUI_DIR}; clone it next to rx "
      "(or set RX_LIBULTRAGUI_DIR). rx::ui draws the engine splash, so every "
      "rx application needs it.")
  endif()
  # We bring our own Vulkan renderer (ugui_backend.cc, dynamic rendering) and
  # window/input, so build ultragui headless: no bundled backends, no GLFW, no
  # bundled shaders, and swap in our own no-op Platform/RHI sources.
  set(ULTRAGUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_BACKEND_VULKAN OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_LUA            OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_AUDIO          OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_LOTTIE         OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_VIDEO          OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_PLATFORM_GLFW  OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_BUILD_SHADERS  OFF CACHE BOOL "" FORCE)
  set(ULTRAGUI_PLATFORM_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ugui_platform.cc"
      CACHE FILEPATH "" FORCE)
  set(ULTRAGUI_RHI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/ugui_rhi.cc"
      CACHE FILEPATH "" FORCE)
  add_subdirectory(${RX_LIBULTRAGUI_DIR} libultragui EXCLUDE_FROM_ALL)
endif()

rx_add_module(ui
  splash.cc
  ugui_backend.cc
)

# ugui types are PUBLIC on rx::ui (splash.h and ugui_backend.h name them), but
# the target itself stays inside the build tree: an installed package re-creates
# it as an imported archive instead (cmake/rxConfig.cmake.in), the same way
# rx::anim treats kinema.
target_link_libraries(rx_ui PUBLIC
  rx::core rx::asset rx::render $<BUILD_INTERFACE:ultragui>)

# The splash plate names the engine build it came from; nothing else in the
# tree needs a version string, so it is defined here rather than generated.
target_compile_definitions(rx_ui PRIVATE RX_ENGINE_VERSION="rx ${PROJECT_VERSION}")

# ugui pipeline shaders (SDF quads, glyphs, frosted glass), hlsl -> embedded spirv.
rx_embed_shaders(rx_ui
  shaders/ugui_quad.vs.hlsl
  shaders/ugui_quad.ps.hlsl
  shaders/ugui_text.vs.hlsl
  shaders/ugui_text.ps.hlsl
  shaders/ugui_frost.vs.hlsl
  shaders/ugui_frost.ps.hlsl)

# The wordmark travels inside the binary, not in an archive next to it: a
# splash that can be missing its logo is not a splash. brand/README.md says
# how the shipped svg differs from the artwork it came from.
set(rx_brand_svg ${CMAKE_CURRENT_SOURCE_DIR}/brand/rx_engine.svg)
set(rx_brand_header ${CMAKE_BINARY_DIR}/generated/ui/brand/rx_engine_svg.h)
set(rx_embed_bytes_script ${PROJECT_SOURCE_DIR}/cmake/embed_bytes.cmake)
add_custom_command(OUTPUT ${rx_brand_header}
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${rx_brand_svg} -DHEADER=${rx_brand_header}
          -DSYMBOL=kRxEngineSvg -P ${rx_embed_bytes_script}
  DEPENDS ${rx_brand_svg} ${rx_embed_bytes_script}
  COMMENT "embed rx_engine.svg"
  VERBATIM)
add_custom_target(rx_ui_brand DEPENDS ${rx_brand_header})
add_dependencies(rx_ui rx_ui_brand)
