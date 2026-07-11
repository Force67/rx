cmake_minimum_required(VERSION 3.24)
project(rx VERSION 0.1.0 LANGUAGES CXX C)

# equilibrium needs C++23, the engine uses it as its container and string layer.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RX_ENABLE_VALIDATION "Enable vulkan validation layers in debug" ON)
option(RX_FETCH_SDL3 "Download SDL3 when not found on the system" OFF)
option(RX_FSR3 "Enable the AMD FSR 3.1 upscaler backend" ON)
option(RX_DLSS "Enable the NVIDIA DLSS upscaler backend" ON)
option(RX_NRD "Enable the NVIDIA NRD ray tracing denoiser" ON)
option(RX_JOLT "Enable Jolt physics" ON)
option(RX_BUILD_TESTS "Build library test suites" ON)
option(RX_BUILD_RUNTIME "Build the rx viewer executable" ON)
option(RX_AUDIO_FFMPEG "Decode compressed audio via FFmpeg (libav*)" OFF)
option(RX_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
# Route the engine's allocations through mimalloc for faster, lower-fragmentation
# heap use. Off by default: opt-in per build, measured before it becomes a
# default. Incompatible with the sanitizer build (ASan owns the allocator).
option(RX_MIMALLOC "Route allocations through mimalloc" OFF)

# install()/find_package(rx) support. Default ON only when rx is the top-level
# project; an add_subdirectory consumer (recreation) leaves it OFF so its build
# is untouched. See cmake/install.cmake and EMBEDDING.md.
option(RX_INSTALL "Generate install()/export rules for find_package(rx)"
  ${PROJECT_IS_TOP_LEVEL})

# The "DLL test": build each rx module as a shared library instead of a static
# archive. Off by default (the static build is the shipping configuration). This
# is a correctness discipline, not a distribution mode: hidden visibility plus
# explicit per-module export annotations (engine/core/export.h) document each
# module's real cross-DSO API and flush out ambient-state bugs. See EMBEDDING.md.
option(RX_SHARED "Build rx modules as shared libraries (experimental)" OFF)

if(RX_SHARED)
  if(RX_INSTALL)
    message(FATAL_ERROR
      "RX_SHARED + RX_INSTALL is unsupported: the install/export rules and the "
      "bundled-archive packaging assume static rx_* archives. Configure with "
      "-DRX_INSTALL=OFF (it defaults off for add_subdirectory consumers).")
  endif()
  set(RX_LIB_TYPE SHARED)
  # Third-party static archives (equilibrium::base, volk, VMA, Jolt, kinema, NRD,
  # FSR3) get linked into rx shared objects, so their objects must be PIC.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  message(WARNING
    "RX_SHARED is experimental. rx modules build as shared objects with hidden "
    "visibility + explicit export annotations. Caveat: equilibrium's base::Option "
    "registry (base::InitChain) is a per-DSO instance under hidden visibility, so "
    "each option-owning DSO re-applies env overrides at init (see EMBEDDING.md). "
    "Env knobs work; the mechanism differs from the static single-registry build.")
else()
  set(RX_LIB_TYPE STATIC)
endif()

# Android is a first-class target, but several desktop backends do not apply:
# DLSS/FSR3/NRD ship x86_64 host libraries (and Adreno has no HW ray tracing or
# mesh shaders, so the raster path is the real path), and SDL3 is replaced by a
# native ANativeWindow backend. Force them off so an Android configure is clean.
if(ANDROID)
  set(RX_DLSS OFF CACHE BOOL "" FORCE)
  set(RX_FSR3 OFF CACHE BOOL "" FORCE)
  set(RX_NRD OFF CACHE BOOL "" FORCE)
  set(RX_FETCH_SDL3 OFF CACHE BOOL "" FORCE)
endif()

# macOS runs Vulkan through MoltenVK (Vulkan-on-Metal). The three GPU-vendor
# backends do not apply: DLSS ships an x86_64 NVIDIA-only host library, and the
# FSR3/NRD shader-permutation toolchains have no validated MoltenVK path. The
# raster path is the real path here, so force them off for a clean Apple
# configure; windowing (SDL3/Metal), physics (Jolt) and the rest stay on.
if(APPLE)
  set(RX_DLSS OFF CACHE BOOL "" FORCE)
  set(RX_FSR3 OFF CACHE BOOL "" FORCE)
  set(RX_NRD OFF CACHE BOOL "" FORCE)
endif()

if(RX_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(RX_FSR3 AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/FidelityFX-SDK/sdk/include/FidelityFX/host/ffx_fsr3upscaler.h)
  message(STATUS "FidelityFX-SDK not found, fsr3 disabled (run tools/get_fidelityfx.sh)")
  set(RX_FSR3 OFF)
endif()

if(RX_DLSS AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/DLSS/lib/Linux_x86_64/libnvsdk_ngx.a)
  message(STATUS "DLSS SDK not found, dlss disabled (run tools/get_dlss.sh)")
  set(RX_DLSS OFF)
endif()

if(RX_NRD AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/NRD/Include/NRD.h)
  message(STATUS "NRD not found, denoiser disabled (run tools/get_nrd.sh)")
  set(RX_NRD OFF)
endif()

if(RX_JOLT AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics/Jolt/Jolt.cmake)
  message(STATUS "JoltPhysics not found, physics disabled (run tools/get_jolt.sh)")
  set(RX_JOLT OFF)
endif()

# Helpers are included by path, never via CMAKE_MODULE_PATH: a parent project
# with same-named modules (warnings/shaders/mimalloc) must not shadow these,
# and rx must not shadow the parent's.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/warnings.cmake)

include(FetchContent)
find_package(ZLIB QUIET)

# Vulkan headers and the volk loader are pinned and fetched, no SDK needed.
# volk dlopens the system vulkan loader at runtime, missing drivers degrade
# the renderer to a stub instead of failing the build.
# A parent project may already vendor volk/VMA; only fetch when it has not.
if(NOT TARGET volk)
  FetchContent_Declare(VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.4.304
    GIT_SHALLOW ON)
  FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 1.4.304
    GIT_SHALLOW ON)
  FetchContent_Declare(VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.1.0
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(VulkanHeaders volk VulkanMemoryAllocator)
  target_link_libraries(volk PUBLIC Vulkan::Headers)
  if(RX_SHARED)
    # volk names its global function-pointer table identically to the real
    # Vulkan entry points (vkEnumerateInstanceExtensionProperties, ...). volk.c
    # already hides the pointer definitions but leaves volkInitialize/volkLoad*
    # at default visibility, so under RX_SHARED those functions get EXPORTED from
    # every DSO that links volk and interpose one another (the filled table and
    # the read table end up in different DSOs -> a null vk* call). Exporting the
    # whole table instead is worse: it collides with SDL3's own Vulkan symbols.
    # The correct model is a fully-private volk copy per DSO that uses it, each
    # initialized by that DSO. Force hidden visibility so no volk symbol leaks
    # out to interpose another DSO or SDL. (render initializes its copy in
    # vk_device.cc; the viewer initializes the imgui copy, see debug_ui.cc.)
    set_target_properties(volk PROPERTIES
      C_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON)
  endif()
  if(ANDROID)
    # So volk's loader compiles the android-surface entry point and consumers see
    # vkCreateAndroidSurfaceKHR / VkAndroidSurfaceCreateInfoKHR.
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR=1)
  endif()
endif()

# Shaders are hlsl, compiled to spirv at build time with dxc and embedded.
find_program(RX_DXC dxc)
if(NOT RX_DXC)
  message(FATAL_ERROR "dxc (DirectXShaderCompiler) is required to compile hlsl to spirv")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/shaders.cmake)

find_package(SDL3 QUIET)
if(NOT SDL3_FOUND AND RX_FETCH_SDL3)
  set(SDL_TEST_LIBRARY OFF)
  # Static keeps the artifacts self contained, no SDL3.dll shipping.
  set(SDL_SHARED OFF)
  set(SDL_STATIC ON)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.16
    GIT_SHALLOW ON)
  FetchContent_MakeAvailable(SDL3)
  set(SDL3_FOUND TRUE)
endif()
if(SDL3_FOUND)
  message(STATUS "sdl3 found, native windowing enabled")
else()
  message(STATUS "sdl3 not found, runtime is headless")
endif()

# Optional mimalloc allocator (defines rx_enable_mimalloc(); no-op when the
# option is off). Included here so the helper exists before the runtime
# executable is declared.
if(RX_MIMALLOC AND RX_SANITIZE)
  message(STATUS "mimalloc disabled: RX_SANITIZE owns the allocator")
  set(RX_MIMALLOC OFF CACHE BOOL "" FORCE)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/mimalloc.cmake)

# equilibrium's base library (third_party/equilibrium, a submodule) supplies the
# base:: containers, strings and atomics every module builds on.
# A parent project may already vendor equilibrium; only add it when it has not.
if(NOT TARGET equilibrium::base)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium/CMakeLists.txt")
    message(FATAL_ERROR
      "equilibrium submodule missing; run: git submodule update --init third_party/equilibrium")
  endif()
  set(EQ_USE_STD_ATOMIC ON CACHE BOOL "" FORCE)
  set(EQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/equilibrium equilibrium EXCLUDE_FROM_ALL)
endif()

add_subdirectory(third_party)

set(RX_MODULES
  core
  ecs
  asset
  scene
  render
  physics
  anim
  audio
  rpc
  app
)

# Self-contained reusable libraries (no engine dependencies).
if(RX_BUILD_TESTS)
  enable_testing()  # before the libs so their add_test calls register
endif()

# kinema: the skeletal animation runtime, its own repo since 2026-07
# (github.com/Force67/kinema), consumed as a sibling checkout like the other
# co-developed libraries. A parent project may already vendor it; only add it
# when the target is absent.
if(NOT TARGET kinema)
  set(RX_KINEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../kinema" CACHE PATH "Path to a kinema checkout")
  if(NOT EXISTS "${RX_KINEMA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "kinema not found at ${RX_KINEMA_DIR}; clone https://github.com/Force67/kinema next to rx (or set RX_KINEMA_DIR)")
  endif()
  set(KINEMA_BUILD_TESTS ${RX_BUILD_TESTS})
  add_subdirectory(${RX_KINEMA_DIR} kinema)
endif()

foreach(module ${RX_MODULES})
  add_subdirectory(engine/${module})
endforeach()

if(RX_BUILD_RUNTIME)
  add_subdirectory(runtime)
endif()

# Offscreen RHI acceptance test: a surfaceless Vulkan device that clears+draws a
# triangle into a GpuImage and reads the pixels back through the RHI. Registered
# like kinematest; skips cleanly (null backend) when no Vulkan driver is present,
# so plain `ctest` passes and a real GPU run under vkrun proves the pixel path.
if(RX_BUILD_TESTS AND RX_RHI_VULKAN)
  add_executable(offscreen_test test/offscreen_test.cc)
  target_link_libraries(offscreen_test PRIVATE rx::render rx::core)
  rx_embed_shaders(offscreen_test
    test/shaders/offscreen_tri.vs.hlsl
    test/shaders/offscreen_tri.ps.hlsl)
  add_test(NAME offscreen_test COMMAND offscreen_test)

  # Acceleration-structure compaction: builds a BLAS with allow_compaction,
  # queries the compacted size across a submit boundary, compact-copies into a
  # tight structure and builds a TLAS over it. Skips (exit 0) with no driver or
  # no ray tracing; the real path runs under vkrun.
  add_executable(compaction_test test/compaction_test.cc)
  target_link_libraries(compaction_test PRIVATE rx::render rx::core)
  add_test(NAME compaction_test COMMAND compaction_test)

  # The same acceptance tests against the d3d12 backend (vkd3d on linux). The
  # tests read RX_RHI to pick the backend and skip cleanly when the driver or
  # feature is unavailable.
  if(RX_RHI_D3D12)
    add_test(NAME offscreen_test_d3d12 COMMAND offscreen_test)
    set_tests_properties(offscreen_test_d3d12 PROPERTIES ENVIRONMENT "RX_RHI=d3d12")
    add_test(NAME compaction_test_d3d12 COMMAND compaction_test)
    set_tests_properties(compaction_test_d3d12 PROPERTIES ENVIRONMENT "RX_RHI=d3d12")
  endif()
endif()

# install()/export rules, generated last so every module target (and the
# RX_INSTALL_* feature flags the modules set) already exists.
if(RX_INSTALL)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/install.cmake)
endif()
