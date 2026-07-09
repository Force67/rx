# Vendored third party code. Kept in tree because the nix build configures
# with FETCHCONTENT_FULLY_DISCONNECTED, so anything fetched needs a flake
# input; small single-header or few-file libraries are simpler vendored.

# Dear ImGui (docking branch) with the SDL3 platform and Vulkan renderer
# backends. The Vulkan backend resolves entry points through volk, matching
# the engine's loader setup. Only built when a window backend exists.
if(SDL3_FOUND)
  add_library(rx_imgui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
  )
  add_library(rx::imgui ALIAS rx_imgui)
  target_include_directories(rx_imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)
  target_compile_definitions(rx_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
  target_link_libraries(rx_imgui PUBLIC SDL3::SDL3 volk)
endif()

add_library(rx_cgltf INTERFACE)
add_library(rx::cgltf ALIAS rx_cgltf)
target_include_directories(rx_cgltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)

add_library(rx_stb INTERFACE)
add_library(rx::stb ALIAS rx_stb)
target_include_directories(rx_stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# AMD FidelityFX FSR 3.1 upscaler, downloaded via tools/get_fidelityfx.sh and
# built with our own CMake (the SDK's is Windows-only).
if(RX_FSR3)
  include(fsr3.cmake)
endif()

# NVIDIA DLSS (NGX) upscaler, downloaded via tools/get_dlss.sh.
if(RX_DLSS)
  include(dlss.cmake)
endif()

# NVIDIA NRD ray tracing denoiser, downloaded via tools/get_nrd.sh.
if(RX_NRD)
  include(nrd.cmake)
endif()

# Jolt physics, downloaded via tools/get_jolt.sh. Builds its own static lib;
# samples/tests/viewer stay off.
if(RX_JOLT)
  set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
  set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
  set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
  set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
  # Jolt defaults to the static MSVC runtime (/MT); the rest of the build uses
  # the dynamic one (/MD, CMake's default), so linking the engine against Jolt
  # otherwise trips LNK2038/LNK2005 runtime-library mismatches. Keep Jolt on /MD.
  set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
  add_subdirectory(JoltPhysics/Build jolt EXCLUDE_FROM_ALL)
  # This Jolt revision's Core.h leans on <type_traits> arriving transitively,
  # which holds under libstdc++ but not libc++ (only some traits leak through) on
  # either the NDK or Apple toolchains. Force-include it so
  # std::is_trivially_constructible resolves, for Jolt's own sources and for
  # engine code that includes Jolt headers (PUBLIC).
  if((ANDROID OR APPLE) AND TARGET Jolt)
    target_compile_options(Jolt PUBLIC -include type_traits)
  endif()
endif()
