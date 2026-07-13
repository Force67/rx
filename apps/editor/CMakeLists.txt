# rx editor: an app::Application on app::Host with its entire UI in libultragui.

# --- libultragui: sibling checkout ../libultragui (discovered like ../kinema) ---
if(NOT TARGET ultragui)
  set(RX_LIBULTRAGUI_DIR "${PROJECT_SOURCE_DIR}/../libultragui"
      CACHE PATH "Path to a libultragui checkout")
  if(NOT EXISTS "${RX_LIBULTRAGUI_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "libultragui not found at ${RX_LIBULTRAGUI_DIR}; clone it next to rx "
      "(or set RX_LIBULTRAGUI_DIR).")
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

add_executable(rx_editor
  main.cc
  editor_app.cc
  editor_ui.cc
  editor_input.cc
  fly_camera.cc
  ugui_backend.cc)

target_link_libraries(rx_editor PRIVATE
  rx::app rx::edit rx::render rx::asset rx::scene rx::ecs rx::core
  ultragui)

target_compile_definitions(rx_editor PRIVATE
  RX_EDITOR_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui")

# ugui pipeline shaders (SDF quads, glyphs, frosted glass), hlsl -> embedded spirv.
rx_embed_shaders(rx_editor
  shaders/ugui_quad.vs.hlsl
  shaders/ugui_quad.ps.hlsl
  shaders/ugui_text.vs.hlsl
  shaders/ugui_text.ps.hlsl
  shaders/ugui_frost.vs.hlsl
  shaders/ugui_frost.ps.hlsl)
