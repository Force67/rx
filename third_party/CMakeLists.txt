# Vendored third party code. Kept in tree because the nix build configures
# with FETCHCONTENT_FULLY_DISCONNECTED, so anything fetched needs a flake
# input; small single-header or few-file libraries are simpler vendored.

# Dear ImGui (docking branch). Split into a platform/API-agnostic core and the
# SDL3 platform + raw-Vulkan render backends:
#   rx_imgui_core - imgui core only (no SDL3, no volk). The engine's RHI imgui
#                   render backend (engine/render/util/imgui_renderer) links this
#                   for the imgui headers/structs, so engine/render pulls in
#                   neither SDL3 nor a second volk copy.
#   rx_imgui      - the SDL3 platform backend plus the raw imgui_impl_vulkan
#                   backend, on top of the core. Kept for apps that still record
#                   imgui through raw Vulkan (recreation's own debug overlay).
# Only built when a window backend exists.
if(SDL3_FOUND)
  add_library(rx_imgui_core STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
  )
  add_library(rx::imgui_core ALIAS rx_imgui_core)
  target_include_directories(rx_imgui_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends)

  add_library(rx_imgui STATIC
    imgui/backends/imgui_impl_sdl3.cpp
    imgui/backends/imgui_impl_vulkan.cpp
  )
  add_library(rx::imgui ALIAS rx_imgui)
  target_compile_definitions(rx_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
  # imgui carries its own volk copy here. Under RX_SHARED volk is compiled hidden
  # (top-level CMakeLists), so this copy is private to the final executable and
  # does not interpose librx_render's copy; the viewer initializes it before
  # imgui draws (see runtime/debug_ui.cc, RX_SHARED_BUILD path). rx_imgui_core is
  # PUBLIC so rx::imgui consumers still get the imgui headers and one core.
  target_link_libraries(rx_imgui PUBLIC rx_imgui_core SDL3::SDL3 volk)
endif()

add_library(rx_cgltf INTERFACE)
add_library(rx::cgltf ALIAS rx_cgltf)
target_include_directories(rx_cgltf INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/cgltf)

add_library(rx_stb INTERFACE)
add_library(rx::stb ALIAS rx_stb)
target_include_directories(rx_stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# stb's implementations compiled exactly once, in their own archive rather than
# inside whichever module happened to want them first. Modules that only read
# the headers keep linking rx::stb; anything that calls stbi_* links this. It
# has to be its own target because tinyusdz calls into it too, and a static
# archive only resolves symbols for the archives that follow it on the link
# line - owning the definitions inside rx_asset put them in the wrong place.
add_library(rx_stb_impl STATIC ${CMAKE_CURRENT_SOURCE_DIR}/stb_impl.c)
add_library(rx::stb_impl ALIAS rx_stb_impl)
target_link_libraries(rx_stb_impl PUBLIC rx_stb)
if(NOT MSVC)
  target_compile_options(rx_stb_impl PRIVATE -w)
endif()

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

# TinyUSDZ, downloaded via tools/get_tinyusdz.sh. The USD reader behind
# engine/asset/usd_loader: crate/ascii/usdz parsing, composition, and Tydra
# (its stage -> renderable-scene converter).
if(RX_USD)
  set(TINYUSDZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_USD_TO_GLTF OFF CACHE BOOL "" FORCE)
  set(TINYUSDZ_USE_CCACHE OFF CACHE BOOL "" FORCE)
  # Tydra is the whole point (it flattens a composed stage into meshes and
  # materials); PCP resolves the reference/payload arcs the NVIDIA scenes use.
  set(TINYUSDZ_WITH_TYDRA ON CACHE BOOL "" FORCE)
  set(TINYUSDZ_WITH_PCP ON CACHE BOOL "" FORCE)
  # The engine has no use for USD audio prims, and it pulls in decoders rx
  # already carries in engine/audio.
  set(TINYUSDZ_WITH_AUDIO OFF CACHE BOOL "" FORCE)
  # tinyusdz vendors the same stb v2.30 rx does, so point it at rx_stb_impl's
  # definitions instead of letting it emit a second copy that collides at link
  # time.
  set(TINYUSDZ_NO_STB_IMAGE_IMPLEMENTATION ON CACHE BOOL "" FORCE)
  set(TINYUSDZ_NO_STB_IMAGE_WRITE_IMPLEMENTATION ON CACHE BOOL "" FORCE)
  add_subdirectory(tinyusdz tinyusdz EXCLUDE_FROM_ALL)
  # tinyusdz only sets its own standard when it is the top-level project; as a
  # subdirectory it would inherit the engine's C++23, which it is not written
  # or tested against. Its headers still compile into rx_asset's C++23 TUs.
  foreach(usd_target tinyusdz_static tinyusdz_subdiv)
    if(TARGET ${usd_target})
      set_target_properties(${usd_target} PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
      if(NOT MSVC)
        # Vendored code we do not patch, and not clean under the engine's
        # warning set.
        target_compile_options(${usd_target} PRIVATE -w)
      endif()
    endif()
  endforeach()
  # tinyusdz needs the stb symbols at link time but must keep including its own
  # copies of the headers, not rx's - hence LINK_ONLY, and hence setting the
  # property directly (tinyusdz's own CMakeLists uses the plain
  # target_link_libraries signature, which cannot be mixed with the keyword one).
  set_property(TARGET tinyusdz_static APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES $<LINK_ONLY:rx_stb_impl>)
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
  # 5.6's compute backends (for Jolt's own GPU hair) drag in a shader
  # toolchain; the engine uses CPU soft bodies and renders hair itself.
  set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
  set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
  set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
  set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
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
