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
option(RX_USD "Enable OpenUSD scene loading (needs tools/get_tinyusdz.sh)" ON)
# Vulkan 1.1 accepts SPIR-V 1.3, or 1.4 with VK_KHR_spirv_1_4; the default
# shader target emits 1.6, which a conformant 1.1 driver rejects outright (only
# NVIDIA's is lenient enough to load it anyway). Turning this on retargets both
# shader toolchains so the modules load on a real 1.1 device. Off by default:
# the desktop build stays on the newer target env.
option(RX_SPIRV_1_4 "Emit SPIR-V 1.4 modules (loadable on Vulkan 1.1)" OFF)
option(RX_BUILD_TESTS "Build library test suites" ON)
option(RX_BUILD_RUNTIME "Build the rx viewer executable" ON)
option(RX_BUILD_EDITOR "Build the rx editor application (apps/editor, needs libultragui)" OFF)
option(RX_BUILD_TOOLS "Build the asset authoring tools (rxpack)" ON)
option(RX_BUILD_EXAMPLES "Build small standalone engine examples" OFF)
option(RX_AUDIO_FFMPEG "Decode compressed audio via FFmpeg (libav*)" OFF)
option(RX_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
# ThreadSanitizer, for the cross-thread paths (deferred destruction from a
# streaming worker, the asset cache). Mutually exclusive with RX_SANITIZE: TSan
# and ASan cannot be linked together.
option(RX_SANITIZE_THREAD "Build with ThreadSanitizer" OFF)
# Route the engine's allocations through mimalloc for faster, lower-fragmentation
# heap use; also enables the per-category memory tracker (the new/delete
# override needs mi_usable_size for exact free accounting). Incompatible with
# the sanitizer build (ASan owns the allocator), where it is forced off.
option(RX_MIMALLOC "Route allocations through mimalloc (+memory tracking)" ON)

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
# Mobile drivers are also the ones that hold Vulkan to 1.1, so the SPIR-V 1.4
# shader target comes on here too.
if(ANDROID)
  set(RX_DLSS OFF CACHE BOOL "" FORCE)
  set(RX_FSR3 OFF CACHE BOOL "" FORCE)
  set(RX_NRD OFF CACHE BOOL "" FORCE)
  set(RX_FETCH_SDL3 OFF CACHE BOOL "" FORCE)
  set(RX_SPIRV_1_4 ON CACHE BOOL "" FORCE)
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

# The three vendored GPU-vendor backends cannot honour RX_SPIRV_1_4: NRD's
# ShaderMake and FSR3's glslang both compile their permutations for Vulkan 1.2,
# so their embedded blobs are SPIR-V 1.5, and DLSS ships as a prebuilt binary
# with no shader step at all. They are gitignored downloads, so there is no
# target-env knob to turn either. Force them off for the same reason Android
# does: they are desktop-vendor features, and no card that runs them is held to
# Vulkan 1.1 in the first place.
if(RX_SPIRV_1_4)
  set(RX_DLSS OFF CACHE BOOL "" FORCE)
  set(RX_FSR3 OFF CACHE BOOL "" FORCE)
  set(RX_NRD OFF CACHE BOOL "" FORCE)
endif()

if(RX_SANITIZE AND RX_SANITIZE_THREAD)
  message(FATAL_ERROR "RX_SANITIZE and RX_SANITIZE_THREAD are mutually exclusive")
endif()
if(RX_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(RX_SANITIZE_THREAD)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
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

if(RX_USD AND NOT EXISTS
   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyusdz/src/tinyusdz.hh)
  message(STATUS "tinyusdz not found, usd loading disabled (run tools/get_tinyusdz.sh)")
  set(RX_USD OFF)
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

# Shaders are hlsl or slang, compiled to spirv at build time (dxc respectively
# slangc) and embedded. slangc is only required when a .slang shader is
# actually registered; rx_embed_shaders errors at that point if it is missing.
find_program(RX_DXC dxc)
if(NOT RX_DXC)
  message(FATAL_ERROR "dxc (DirectXShaderCompiler) is required to compile hlsl to spirv")
endif()
find_program(RX_SLANGC slangc)
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
if(RX_MIMALLOC AND (RX_SANITIZE OR RX_SANITIZE_THREAD))
  message(STATUS "mimalloc disabled: the sanitizer owns the allocator")
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

# Overridable so special-purpose builds (e.g. the mingw cross-compile that
# packages the d3d12 backend tests for a Windows runtime) can build a slice.
if(NOT DEFINED RX_MODULES)
  set(RX_MODULES
    core
    ecs
    script
    asset
    scene
    terrain
    edit
    world
    render
    render2d
    weather
    placement
    physics
    anim
    audio
    rpc
    authoring
    net
    nav
    character
    combat
    inventory
    locomotion
    app
  )
endif()

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

# zetanet: the UDP transport rx::net's sessions ride, consumed as an optional
# sibling checkout. A parent project may already provide the target; without
# either, rx::net still builds its transport-free half (codec, replication,
# interest bubbles) and only the sessions are absent. Zetanet hard-codes
# add_subdirectory(vendor/equilibrium); rather than patch upstream,
# add_subdirectory is locally overridden so that one call reuses the
# equilibrium::base vendored above instead of compiling a second, conflicting
# copy.
if(NOT TARGET zetanet AND "net" IN_LIST RX_MODULES)
  set(RX_ZETANET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zetanet" CACHE PATH
    "Path to a zetanet checkout (develop branch); missing disables the net sessions")
  if(EXISTS "${RX_ZETANET_DIR}/CMakeLists.txt")
    set(ZNET_USE_STL OFF CACHE BOOL "" FORCE)
    set(ZNET_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    function(add_subdirectory _dir)
      get_filename_component(_leaf "${_dir}" NAME)
      if(_leaf STREQUAL "equilibrium" AND TARGET equilibrium::base)
        return()  # zetanet's vendored copy: reuse the one vendored above
      endif()
      _add_subdirectory("${_dir}" ${ARGN})
    endfunction()
    add_subdirectory(${RX_ZETANET_DIR} zetanet EXCLUDE_FROM_ALL)
  else()
    message(STATUS "rx: no zetanet at ${RX_ZETANET_DIR}, net sessions disabled")
  endif()
endif()

# A downstream fork registers its own engine/<name> modules by shipping this
# file: it runs with RX_MODULES already populated, so it appends to that list
# (and sets whatever paths its modules need) without editing anything upstream
# owns. Nothing in this tree provides one, which is the point - a merge from
# here can never touch it, so a fork carrying private modules stays mergeable
# with no conflict in the shared build files.
include("${CMAKE_CURRENT_SOURCE_DIR}/engine/local.cmake" OPTIONAL)

foreach(module ${RX_MODULES})
  add_subdirectory(engine/${module})
endforeach()

if(RX_BUILD_EXAMPLES AND "anim" IN_LIST RX_MODULES)
  add_subdirectory(examples)
endif()

# rx::render2d CPU tests: camera transforms, isometric picking and tilemap data
# rules need no GPU or window, so they run on every build host.
if(RX_BUILD_TESTS AND "render2d" IN_LIST RX_MODULES)
  add_executable(render2d_test test/render2d_test.cc)
  target_link_libraries(render2d_test PRIVATE rx::render2d)
  add_test(NAME render2d_test COMMAND render2d_test)
endif()

# rx::net unit tests: pure logic (wire codec, per-peer delta streams, bubble
# membership + ownership), no GPU, driver or transport needed, so plain
# `ctest` runs them everywhere.
if(RX_BUILD_TESTS AND "net" IN_LIST RX_MODULES)
  add_executable(net_test test/net_test.cc)
  target_link_libraries(net_test PRIVATE rx::net rx::core)
  add_test(NAME net_test COMMAND net_test)
endif()

# Touch contact state machine: header-only logic over core/input.h, so it needs
# no window or panel and covers the lifecycle rules a touchscreen would
# otherwise be the only way to exercise.
if(RX_BUILD_TESTS)
  add_executable(input_touch_test test/input_touch_test.cc)
  target_link_libraries(input_touch_test PRIVATE rx::core)
  add_test(NAME input_touch_test COMMAND input_touch_test)

  # The other half: SDL's own event queue driven through the real backend, so
  # the normalized-to-pixel translation is covered too. Skips without a display.
  if(TARGET SDL3::SDL3)
    add_executable(input_touch_sdl_test test/input_touch_sdl_test.cc)
    target_link_libraries(input_touch_sdl_test PRIVATE rx::core SDL3::SDL3)
    add_test(NAME input_touch_sdl_test COMMAND input_touch_sdl_test)
  endif()
endif()

# rx::script unit tests: the portable value/string primitives and the handler
# registry driven end-to-end against a real ecs::World, with no runtime, GPU or
# transport, so plain `ctest` runs them everywhere.
if(RX_BUILD_TESTS AND "script" IN_LIST RX_MODULES)
  add_executable(script_test test/script_test.cc)
  target_link_libraries(script_test PRIVATE rx::scene rx::script rx::ecs rx::core)
  add_test(NAME script_test COMMAND script_test)
endif()

# rx::authoring unit tests: the rpc <-> script marshalling, the trust gate, and
# the unix socket endpoint driven with a real client fd. No GPU or window, but
# unix sockets, so it stays off Windows like the endpoint does.
if(RX_BUILD_TESTS AND NOT WIN32 AND "authoring" IN_LIST RX_MODULES)
  add_executable(command_bridge_test test/command_bridge_test.cc)
  target_link_libraries(command_bridge_test PRIVATE rx::authoring rx::scene rx::script
    rx::rpc rx::ecs rx::core)
  add_test(NAME command_bridge_test COMMAND command_bridge_test)
endif()

# rx::nav unit tests: pure CPU logic over a synthetic terrain sampler (costed
# A*, entry-cost commitment, funnel, event repathing, position scoring), so
# plain `ctest` runs them everywhere.
if(RX_BUILD_TESTS AND "nav" IN_LIST RX_MODULES)
  add_executable(nav_test test/nav_test.cc)
  target_link_libraries(nav_test PRIVATE rx::nav rx::scene rx::ecs rx::core)
  add_test(NAME nav_test COMMAND nav_test)
endif()

# rx::weather unit tests: pure CPU logic (deterministic scheduling, region
# restriction, transition blending, scripted override, surface + wind + lightning
# integrals), no GPU, so plain `ctest` runs them everywhere.
if(RX_BUILD_TESTS AND "weather" IN_LIST RX_MODULES)
  add_executable(weather_test test/weather_test.cc)
  target_link_libraries(weather_test PRIVATE rx::weather rx::core)
  add_test(NAME weather_test COMMAND weather_test)
endif()

if(RX_BUILD_RUNTIME)
  add_subdirectory(runtime)
endif()

# The editor application: an app::Application whose entire UI is libultragui.
if(RX_BUILD_EDITOR)
  add_subdirectory(apps/editor)
endif()

# Asset authoring tools. rxpack creates/lists/extracts/verifies .rxp game
# archives (engine/asset/pack.h).
if(RX_BUILD_TOOLS)
  add_executable(rxpack tools/rxpack.cc)
  target_link_libraries(rxpack PRIVATE rx::asset rx::core)
  rx_set_warnings(rxpack)
endif()

# rxdiff compares two rendered pngs against a measured tolerance, which is the
# only comparison a renderer whose radiance cache races its own waves admits
# (a capture's clock is locked, so nothing else about it wanders). Its own
# tool rather than an rx flag: it needs no device, no window and no scene, so it
# links stb and nothing else and starts instantly in a CI loop.
if(RX_BUILD_TOOLS)
  add_executable(rxdiff tools/rxdiff.cc)
  target_link_libraries(rxdiff PRIVATE rx::stb_impl)
  rx_set_warnings(rxdiff)

  # The tolerance itself came from measuring the renderer (the tool's header
  # carries the numbers); these only pin the two ends nothing should ever move.
  # Two checked-in captures of the same Cornell box from different gi paths
  # stand in for a regression, so no new binaries land in the tree for this.
  if(RX_BUILD_TESTS)
    add_test(NAME rxdiff_identical COMMAND rxdiff
      ${CMAKE_CURRENT_SOURCE_DIR}/docs/images/rcgi-cornell-ddgi.png
      ${CMAKE_CURRENT_SOURCE_DIR}/docs/images/rcgi-cornell-ddgi.png)
    add_test(NAME rxdiff_regression COMMAND rxdiff
      ${CMAKE_CURRENT_SOURCE_DIR}/docs/images/rcgi-cornell-ddgi.png
      ${CMAKE_CURRENT_SOURCE_DIR}/docs/images/rcgi-cornell-rcgi.png)
    # Matching the verdict, not just a nonzero exit, so an io error cannot pass
    # for a detected regression.
    set_tests_properties(rxdiff_regression PROPERTIES PASS_REGULAR_EXPRESSION "FAIL")
  endif()
endif()

# rxcall issues one command to a running engine's --authoring-endpoint socket.
# It links rx::rpc rather than reimplementing the frame, so the client and the
# engine can never disagree about the encoding. Unix sockets only, like the
# endpoint itself.
if(RX_BUILD_TOOLS AND NOT WIN32 AND "rpc" IN_LIST RX_MODULES)
  add_executable(rxcall tools/rxcall.cc)
  target_link_libraries(rxcall PRIVATE rx::rpc rx::core)
  rx_set_warnings(rxcall)
endif()

# The engine's own archives, packed from engine/assets with the tool above and
# mounted by asset::MountEngineArchives (one namespace each). Cross builds skip
# this: rxpack would be built for the target and cannot run here.
if(RX_BUILD_TOOLS AND NOT CMAKE_CROSSCOMPILING)
  file(GLOB_RECURSE RX_ENGINE_FONT_FILES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/fonts/*)
  add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/rx_fonts.rxp
    COMMAND rxpack create ${PROJECT_BINARY_DIR}/rx_fonts.rxp
            ${CMAKE_CURRENT_SOURCE_DIR}/engine/assets/fonts
    DEPENDS rxpack ${RX_ENGINE_FONT_FILES}
    COMMENT "Packing rx_fonts.rxp")
  add_custom_target(rx_engine_archives ALL DEPENDS ${PROJECT_BINARY_DIR}/rx_fonts.rxp)
endif()

# Vfs mount points + .rxp pack round trip, pure CPU: no driver or window
# needed, runs everywhere plain `ctest` runs.
if(RX_BUILD_TESTS)
  add_executable(vfs_test test/vfs_test.cc)
  target_link_libraries(vfs_test PRIVATE rx::asset rx::core)
  add_test(NAME vfs_test COMMAND vfs_test)

  add_executable(asset_database_test test/asset_database_test.cc)
  target_link_libraries(asset_database_test PRIVATE rx::asset rx::core)
  add_test(NAME asset_database_test COMMAND asset_database_test)

  # Procedural texture synthesis and the blockout primitives behind the
  # .rxscene Pattern/Shape components.
  add_executable(procedural_asset_test test/procedural_asset_test.cc)
  target_link_libraries(procedural_asset_test PRIVATE rx::asset rx::core)
  add_test(NAME procedural_asset_test COMMAND procedural_asset_test)
endif()

if(RX_BUILD_TESTS AND "asset" IN_LIST RX_MODULES)
  add_executable(gltf_skin_test test/gltf_skin_test.cc)
  target_link_libraries(gltf_skin_test PRIVATE rx::asset rx::core)
  add_test(NAME gltf_skin_test COMMAND gltf_skin_test)

  add_executable(gltf_texture_transform_test test/gltf_texture_transform_test.cc)
  target_link_libraries(gltf_texture_transform_test PRIVATE rx::asset rx::core)
  add_test(NAME gltf_texture_transform_test COMMAND gltf_texture_transform_test)

  add_executable(materialx_test test/materialx_test.cc)
  target_link_libraries(materialx_test PRIVATE rx::asset rx::core)
  add_test(NAME materialx_test COMMAND materialx_test)

  # BCn block encoders and the import-time compressor, against decoders written
  # from the format definitions. No gpu involved, so this one really runs under
  # a plain ctest.
  add_executable(bc_encode_test test/bc_encode_test.cc)
  target_link_libraries(bc_encode_test PRIVATE rx::asset rx::core)
  add_test(NAME bc_encode_test COMMAND bc_encode_test)
endif()

# USD import: composition (a reference arc into a second layer), the z-up /
# metersPerUnit normalization and the UsdPreviewSurface mapping, on a usda
# fixture the test writes itself.
if(RX_BUILD_TESTS AND RX_USD AND "asset" IN_LIST RX_MODULES)
  add_executable(usd_scene_test test/usd_scene_test.cc)
  target_link_libraries(usd_scene_test PRIVATE rx::asset rx::core)
  add_test(NAME usd_scene_test COMMAND usd_scene_test)
endif()

# Procedural vehicle audio: engine/skid/wind synthesis, the SynthVoice seam and
# the VehicleAudio driver through the mixer. Pure CPU (no device), always runs.
if(RX_BUILD_TESTS)
  add_executable(vehicle_audio_test test/vehicle_audio_test.cc)
  target_link_libraries(vehicle_audio_test PRIVATE rx::audio rx::core)
  add_test(NAME vehicle_audio_test COMMAND vehicle_audio_test)
endif()

if(RX_BUILD_TESTS AND "audio" IN_LIST RX_MODULES)
  add_executable(thunder_synth_test test/thunder_synth_test.cc)
  target_link_libraries(thunder_synth_test PRIVATE rx::audio rx::core)
  add_test(NAME thunder_synth_test COMMAND thunder_synth_test)
endif()

# Physics-driven fixed-wing aircraft over the public PhysicsWorld force API:
# ground roll + rotation + climb, overloaded (over-MTOM) degradation, stall
# sink, flap-shortened takeoff, braking, rudder yaw, and NaN-free tumbling.
# Headless (no GPU), needs the Jolt-enabled physics build like vehicle_test.
if(RX_BUILD_TESTS AND RX_JOLT)
  add_executable(aircraft_test test/aircraft_test.cc)
  target_link_libraries(aircraft_test PRIVATE rx::physics rx::core)
  add_test(NAME aircraft_test COMMAND aircraft_test)
endif()

# Cross-system vehicle integration: one PhysicsWorld hosts a car, a boat and an
# aircraft at once for 30 s over a shared heightfield + Gerstner lake, with every
# vehicle's telemetry driving a real EngineSynth. Headless (no GPU/device), needs
# Jolt for the physics path (like aircraft_test/vehicle_test/boat_test).
if(RX_BUILD_TESTS AND RX_JOLT)
  add_executable(vehicle_integration_test test/vehicle_integration_test.cc)
  target_link_libraries(vehicle_integration_test PRIVATE rx::physics rx::audio rx::core)
  add_test(NAME vehicle_integration_test COMMAND vehicle_integration_test)
endif()

# Editor foundations test: reflection, scene round-trip, undo/redo and hierarchy
# composition. Fully headless (no GPU), so it always runs under plain ctest.
if(RX_BUILD_TESTS)
  add_executable(edit_test test/edit_test.cc)
  target_link_libraries(edit_test PRIVATE rx::edit rx::scene rx::ecs rx::asset rx::core)
  add_test(NAME edit_test COMMAND edit_test)

  # The .rxscene authoring passes themselves (runtime/scene_authoring.cc, which
  # is viewer policy rather than a module, so it is compiled in here rather than
  # linked). `rx --validate` covers what a scene REFUSES; this covers what the
  # numbers come out as, which no exit code can show. Null renderer throughout,
  # so it needs no device even though it links rx::render for the signatures.
  add_executable(scene_authoring_test test/scene_authoring_test.cc runtime/scene_authoring.cc)
  target_include_directories(scene_authoring_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(scene_authoring_test
    PRIVATE rx::edit rx::scene rx::ecs rx::asset rx::render rx::core)
  add_test(NAME scene_authoring_test COMMAND scene_authoring_test)
endif()

if(RX_BUILD_TESTS)
  add_executable(camera_test test/camera_test.cc)
  target_link_libraries(camera_test PRIVATE rx::scene rx::ecs rx::core)
  add_test(NAME camera_test COMMAND camera_test)

  add_executable(camera_rig_test test/camera_rig_test.cc)
  target_link_libraries(camera_rig_test PRIVATE rx::scene rx::ecs rx::core)
  add_test(NAME camera_rig_test COMMAND camera_rig_test)

  # Vehicle realism over the real Jolt vehicle path: acceleration + auto-shift,
  # surface-dependent braking (ice vs asphalt), rain wetness, manual gearbox,
  # telemetry ranges and water aquaplaning. Headless (no GPU), needs Jolt.
  if(RX_JOLT)
    add_executable(vehicle_test test/vehicle_test.cc)
    target_link_libraries(vehicle_test PRIVATE rx::physics rx::core)
    add_test(NAME vehicle_test COMMAND vehicle_test)
  endif()

  # Handling profiles: measured, ordered proof that the six presets
  # (engine/physics/vehicle_profiles.h) drive differently - 0-100, braking,
  # roll, grass launch, under/oversteer and downforce. Headless, needs Jolt.
  if(RX_JOLT)
    add_executable(handling_test test/handling_test.cc)
    target_link_libraries(handling_test PRIVATE rx::physics rx::core)
    add_test(NAME handling_test COMMAND handling_test)
  endif()

  # Physics-driven boat over the force-based hull model: floating draft,
  # acceleration, steering, drag decay, planing, river drift, self-righting,
  # buoyancy-exemption and NaN-free chop. Headless (no GPU), needs Jolt.
  if(RX_JOLT)
    add_executable(boat_test test/boat_test.cc)
    target_link_libraries(boat_test PRIVATE rx::physics rx::core)
    add_test(NAME boat_test COMMAND boat_test)
  endif()

  # Boat-type profiles + cargo load: measured, ordered proof that the five presets
  # (engine/physics/boat_profiles.h) float and handle differently and that cargo
  # sinks the hull deeper, slows accel/turning, delays planing and eats freeboard
  # - settle-draft/top-speed/turn-rate orderings, capsize vs self-right, an
  # overloaded barge near awash, and NaN-free chop. Headless, needs Jolt.
  if(RX_JOLT)
    add_executable(boat_profiles_test test/boat_profiles_test.cc)
    target_link_libraries(boat_profiles_test PRIVATE rx::physics rx::core)
    add_test(NAME boat_profiles_test COMMAND boat_profiles_test)
  endif()

  # Physics-driven tethered kite over the force-based flat-plate sail model:
  # launch + stable overhead equilibrium in steady wind, descent + settle in dead
  # air, left/right steering displacement, reel-in altitude change, tow-launch on
  # apparent wind and NaN-free / tension-capped gusting. Headless, needs Jolt.
  if(RX_JOLT)
    add_executable(kite_test test/kite_test.cc)
    target_link_libraries(kite_test PRIVATE rx::physics rx::core)
    add_test(NAME kite_test COMMAND kite_test)
  endif()

  add_executable(world_streaming_test test/world_streaming_test.cc)
  target_link_libraries(world_streaming_test PRIVATE rx::scene rx::ecs rx::core)
  add_test(NAME world_streaming_test COMMAND world_streaming_test)

  if("world" IN_LIST RX_MODULES)
    add_executable(world_format_test test/world_format_test.cc)
    target_link_libraries(world_format_test PRIVATE rx::world rx::core)
    add_test(NAME world_format_test COMMAND world_format_test)

    add_executable(world_map_test test/world_map_test.cc)
    target_link_libraries(world_map_test PRIVATE rx::world rx::asset rx::scene rx::core)
    add_test(NAME world_map_test COMMAND world_map_test)

    add_executable(world_stream_test test/world_stream_test.cc)
    target_link_libraries(world_stream_test PRIVATE rx::world rx::edit rx::asset rx::scene rx::ecs rx::core)
    add_test(NAME world_stream_test COMMAND world_stream_test)

    add_executable(world_overlay_test test/world_overlay_test.cc)
    target_link_libraries(world_overlay_test PRIVATE rx::world rx::core)
    add_test(NAME world_overlay_test COMMAND world_overlay_test)
  endif()

  if("terrain" IN_LIST RX_MODULES)
    add_executable(terrain_test test/terrain_test.cc)
    target_link_libraries(terrain_test PRIVATE rx::terrain rx::asset rx::scene rx::core)
    add_test(NAME terrain_test COMMAND terrain_test)
  endif()

  if("placement" IN_LIST RX_MODULES)
    add_executable(placement_test test/placement_test.cc)
    target_link_libraries(placement_test PRIVATE rx::placement rx::core)
    add_test(NAME placement_test COMMAND placement_test)
  endif()

  add_executable(cloth_collision_test
    test/cloth_collision_test.cc
    engine/physics/cloth_collision.cc)
  target_link_libraries(cloth_collision_test PRIVATE rx::core)
  add_test(NAME cloth_collision_test COMMAND cloth_collision_test)

  add_executable(instance_store_test
    test/instance_store_test.cc
    engine/render/geometry/instance_store.cc)
  target_include_directories(instance_store_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(instance_store_test PRIVATE rx::core)
  add_test(NAME instance_store_test COMMAND instance_store_test)

  add_executable(settings_ini_test
    test/settings_ini_test.cc
    engine/render/core/settings_ini.cc)
  target_include_directories(settings_ini_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_compile_definitions(settings_ini_test PRIVATE RX_RENDER_IMPLEMENTATION)
  target_link_libraries(settings_ini_test PRIVATE rx::core)
  add_test(NAME settings_ini_test COMMAND settings_ini_test)

  add_executable(procedural_grass_test test/procedural_grass_test.cc)
  target_include_directories(procedural_grass_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(procedural_grass_test PRIVATE rx::core)
  add_test(NAME procedural_grass_test COMMAND procedural_grass_test)

  # Solid-angle TLAS instance culler: the distance/angle predicate, the
  # time-sliced group sweep, teleport accept-all and generation invalidation.
  # Pure CPU logic (no GPU), so it always runs under plain ctest.
  add_executable(rt_instance_cull_test
    test/rt_instance_cull_test.cc
    engine/render/gi/rt_instance_cull.cc)
  target_include_directories(rt_instance_cull_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(rt_instance_cull_test PRIVATE rx::core)
  add_test(NAME rt_instance_cull_test COMMAND rt_instance_cull_test)

  # RCGI leak/occlusion hardening helpers: interior-volume classification and the
  # probe-relocation offset packing round trip (header-only, mirrors the shader).
  add_executable(rcgi_interior_test test/rcgi_interior_test.cc)
  target_include_directories(rcgi_interior_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(rcgi_interior_test PRIVATE rx::core)
  add_test(NAME rcgi_interior_test COMMAND rcgi_interior_test)

  add_executable(rcgi_history_test test/rcgi_history_test.cc)
  target_include_directories(rcgi_history_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(rcgi_history_test PRIVATE rx::core)
  add_test(NAME rcgi_history_test COMMAND rcgi_history_test)

  # Async-TLAS slot validity bookkeeping: build/read slot selection, unbuilt-slot
  # fallback, and BLAS-revision invalidation. Header-only pure logic (no GPU).
  add_executable(rt_slot_tracker_test test/rt_slot_tracker_test.cc)
  target_include_directories(rt_slot_tracker_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(rt_slot_tracker_test PRIVATE rx::core)
  add_test(NAME rt_slot_tracker_test COMMAND rt_slot_tracker_test)

# CPU-side memory infrastructure test (tracker/arena/chunk pool/small vector/
# config). rx_enable_mimalloc gives it the tracked new/delete override so the
# tracker assertions run against the real allocation path.
  add_executable(memory_test test/memory_test.cc)
  target_link_libraries(memory_test PRIVATE rx::core)
  rx_enable_mimalloc(memory_test)
  add_test(NAME memory_test COMMAND memory_test)

  # Chunked ECS storage: spawn storms, non-POD relocation, chunk reclamation.
  add_executable(ecs_test test/ecs_test.cc)
  target_link_libraries(ecs_test PRIVATE rx::ecs rx::core)
  rx_enable_mimalloc(ecs_test)
  add_test(NAME ecs_test COMMAND ecs_test)
endif()

# Offscreen RHI acceptance test: a surfaceless Vulkan device that clears+draws a
# triangle into a GpuImage and reads the pixels back through the RHI. Registered
# like kinematest; skips cleanly (null backend) when no Vulkan driver is present,
# so plain `ctest` passes and a real GPU run under vkrun proves the pixel path.
if(RX_BUILD_TESTS AND (RX_RHI_VULKAN OR RX_RHI_D3D12))
  add_executable(adaptive_water_test test/adaptive_water_test.cc)
  target_include_directories(adaptive_water_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium)
  target_link_libraries(adaptive_water_test PRIVATE rx::core)
  add_test(NAME adaptive_water_test COMMAND adaptive_water_test)

  # Texture-space decal baking: bake a stamp into a receiver's tile, read the
  # atlas back, then evict the tile and prove the journal replays it. Skips the
  # GPU half without a driver (the projector math still runs).
  add_executable(decal_bake_test test/decal_bake_test.cc)
  target_link_libraries(decal_bake_test PRIVATE rx::render rx::core)
  add_test(NAME decal_bake_test COMMAND decal_bake_test)

  add_executable(offscreen_test test/offscreen_test.cc)
  target_link_libraries(offscreen_test PRIVATE rx::render rx::core)
  rx_embed_shaders(offscreen_test
    test/shaders/offscreen_tri.vs.slang
    test/shaders/offscreen_tri.ps.slang)
  add_test(NAME offscreen_test COMMAND offscreen_test)

  # Procedural placement: runs the DENSITYMAP/GENERATE/PLACEMENT compute
  # pipeline offscreen and compares the resulting instances against the CPU
  # reference generator (same tiles, same seeds). Skips without a driver.
  if("placement" IN_LIST RX_MODULES)
    add_executable(placement_gpu_test test/placement_gpu_test.cc)
    target_link_libraries(placement_gpu_test PRIVATE rx::placement_gpu rx::placement
      rx::render rx::core)
    add_test(NAME placement_gpu_test COMMAND placement_gpu_test)
  endif()

  # Heightfield fluid solver: fills a bowl behind a ridge, steps the GPU sim,
  # checks volume conservation + no NaNs, then removes the ridge and asserts the
  # dam-break flood settles level. Skips (null backend) with no driver; the real
  # GPU path runs under vkrun.
  add_executable(fluid_sim_test test/fluid_sim_test.cc)
  target_link_libraries(fluid_sim_test PRIVATE rx::render rx::core)
  add_test(NAME fluid_sim_test COMMAND fluid_sim_test)

  # Acceleration-structure compaction: builds a BLAS with allow_compaction,
  # queries the compacted size across a submit boundary, compact-copies into a
  # tight structure and builds a TLAS over it. Skips (exit 0) with no driver or
  # no ray tracing; the real path runs under vkrun.
  add_executable(compaction_test test/compaction_test.cc)
  target_link_libraries(compaction_test PRIVATE rx::render rx::core)
  add_test(NAME compaction_test COMMAND compaction_test)

  # Cull bounding radius: runs cull.cs over a rotated, non-uniformly scaled
  # instance straddling a frustum plane, the one case where reading the model
  # matrix by row instead of by column under-reports the radius and culls
  # visible geometry. Skips (exit 0) with no driver; the real path runs under
  # vkrun.
  add_executable(cull_bounds_test test/cull_bounds_test.cc)
  target_link_libraries(cull_bounds_test PRIVATE rx::render rx::core)
  target_include_directories(cull_bounds_test PRIVATE ${CMAKE_BINARY_DIR}/generated)
  add_test(NAME cull_bounds_test COMMAND cull_bounds_test)

  # Upload batch: buffers created inside Begin/FlushUploadBatch (nested scopes,
  # implicit flush via ImmediateSubmit, soft-budget auto-submit) verified by
  # readback. Skips (exit 0) with no driver; the real path runs under vkrun.
  add_executable(upload_batch_test test/upload_batch_test.cc)
  target_link_libraries(upload_batch_test PRIVATE rx::render rx::core)
  add_test(NAME upload_batch_test COMMAND upload_batch_test)
  # Validation on, and any VUID fails the test: the deferred-destroy case can
  # only report an early free as "destroyed while in use", not as a bad readback.
  set_tests_properties(upload_batch_test PROPERTIES
    ENVIRONMENT "RX_VALIDATION=1"
    FAIL_REGULAR_EXPRESSION "VUID")

  # Morph targets: glTF import + weight-track sampling against the committed
  # CC0 Khronos AnimatedMorphCube, and the shared morph.hlsli accumulation
  # replayed in a compute shader against a CPU reference. The GPU section is
  # Vulkan-only (buffer-device-address reads, like the skinned path) and skips
  # without a driver.
  add_executable(morph_test test/morph_test.cc)
  target_link_libraries(morph_test PRIVATE rx::render rx::anim rx::asset rx::core)
  set(RX_SHADER_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/render/shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/render/shaders/pipeline)
  set(RX_SHADER_NO_DXIL morph_apply.cs.hlsl)
  set(RX_SHADER_DEPS_morph_apply_cs_hlsl engine/render/shaders/pipeline/morph.hlsli)
  rx_embed_shaders(morph_test test/shaders/morph_apply.cs.hlsl)
  unset(RX_SHADER_INCLUDE_DIRS)
  unset(RX_SHADER_NO_DXIL)
  add_test(NAME morph_test
           COMMAND morph_test ${CMAKE_CURRENT_SOURCE_DIR}/test/data/AnimatedMorphCube.glb)

  # Expression controller: pose transitions converge and stay C1 through a
  # mid-flight retarget, outputs stay clamped, seeded blinks land inside their
  # window (and are absorbed by a closed-eyes pose) and a fixed seed replays
  # bit-identically. Pure CPU.
  add_executable(expression_test test/expression_test.cc)
  target_link_libraries(expression_test PRIVATE rx::anim rx::asset rx::core)
  add_test(NAME expression_test COMMAND expression_test)

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

# Extensible soft-tissue body dynamics: physical equilibrium/inertial lag,
# analytical frame-rate independence, anatomical clamps + teleport reset,
# bone layering, source-unit conversion, region presets, and additive
# deformation morph output. Pure CPU and independent of Jolt/the render backend.
if(RX_BUILD_TESTS AND "anim" IN_LIST RX_MODULES)
  add_executable(walk_style_test test/walk_style_test.cc)
  target_link_libraries(walk_style_test PRIVATE rx::anim rx::asset rx::core)
  add_test(NAME walk_style_test COMMAND walk_style_test)

  add_executable(body_dynamics_test test/body_dynamics_test.cc)
  target_link_libraries(body_dynamics_test PRIVATE rx::anim rx::asset rx::core)
  add_test(NAME body_dynamics_test COMMAND body_dynamics_test)

  # Optional end-to-end authoring import test. The normal CTest invocation
  # checks error handling without requiring Blender; pass a .blend path to the
  # executable to convert it and verify its real skin + jiggle response.
  add_executable(blend_jiggle_test test/blend_jiggle_test.cc)
  target_link_libraries(blend_jiggle_test PRIVATE rx::anim rx::asset rx::core)
  target_compile_definitions(blend_jiggle_test PRIVATE
    RX_BLEND_CONVERTER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/tools/blend_to_glb.py")
  add_test(NAME blend_jiggle_api_test COMMAND blend_jiggle_test)
endif()

# Strand-hair simulation: builds the braid + ponytail test grooms, steps the
# Jolt soft-body strand sim under a static and a moving head transform, and
# asserts pins, cross-strand binds and head collision hold. Headless, no GPU.
if(RX_BUILD_TESTS AND RX_JOLT)
  add_executable(hair_sim_test test/hair_sim_test.cc)
  target_link_libraries(hair_sim_test PRIVATE rx::render rx::physics rx::core)
  add_test(NAME hair_sim_test COMMAND hair_sim_test)

  add_executable(cloth_sim_test test/cloth_sim_test.cc)
  target_link_libraries(cloth_sim_test PRIVATE rx::physics rx::core)
  add_test(NAME cloth_sim_test COMMAND cloth_sim_test)
  add_test(NAME cloth_feature_off_test COMMAND cloth_sim_test --feature-off)
  set_tests_properties(cloth_feature_off_test PROPERTIES ENVIRONMENT "RX_FEATURES=-physics.cloth")
endif()

# rx::inventory: stacking/capacity/weight, transfer, equip/unequip, then the
# world-item lifecycle on real Jolt (drop -> settle -> pick up / hibernate /
# wake) and save/load roundtrips for inventories AND world items. Headless.
if(RX_BUILD_TESTS AND RX_JOLT AND "inventory" IN_LIST RX_MODULES)
  add_executable(inventory_test test/inventory_test.cc)
  target_link_libraries(inventory_test PRIVATE
    rx::inventory rx::inventory_world rx::physics rx::scene rx::ecs rx::core)
  add_test(NAME inventory_test COMMAND inventory_test)
endif()

# rx::character locomotion + view modes over the real Jolt character controller:
# gait acceleration, crouch capsule/eye blend, ceiling-blocked uncrouch, jump +
# land, step-up vs blocked, third-person camera obstruction pull-in and the
# FP<->TP view-mode stack transition. Headless (no GPU), needs Jolt.
if(RX_BUILD_TESTS AND RX_JOLT AND "character" IN_LIST RX_MODULES)
  add_executable(character_test test/character_test.cc)
  target_link_libraries(character_test PRIVATE rx::character rx::physics rx::scene rx::ecs rx::core)
  add_test(NAME character_test COMMAND character_test)
endif()

# rx::combat over real Jolt: fire rate and modes, magazine/reserve reloading,
# spread bloom, view recoil kick + recovery, hitscan falloff/zone/penetration,
# ballistic projectiles, explosion falloff and line of sight. Headless.
if(RX_BUILD_TESTS AND RX_JOLT AND "combat" IN_LIST RX_MODULES)
  add_executable(combat_test test/combat_test.cc)
  target_link_libraries(combat_test PRIVATE rx::combat rx::physics rx::scene rx::ecs rx::core)
  add_test(NAME combat_test COMMAND combat_test)
endif()

# rx::locomotion pure math (gait clock, capture point, step clamping, swing
# trajectory, leg IK): no Jolt, no GPU, plain ctest everywhere.
if(RX_BUILD_TESTS AND "locomotion" IN_LIST RX_MODULES)
  add_executable(locomotion_math_test test/locomotion_math_test.cc)
  target_link_libraries(locomotion_math_test PRIVATE rx::locomotion rx::core)
  add_test(NAME locomotion_math_test COMMAND locomotion_math_test)
endif()

# rx::physics body/joint adapter surface the locomotion controller relies on:
# velocity/mass/COM readback, force + torque application, watched contacts,
# motor torque limits. Headless, needs Jolt.
if(RX_BUILD_TESTS AND RX_JOLT)
  add_executable(physics_body_test test/physics_body_test.cc)
  target_link_libraries(physics_body_test PRIVATE rx::physics rx::core)
  add_test(NAME physics_body_test COMMAND physics_body_test)
endif()

# rx::locomotion over real Jolt: rig build sanity, joint motor target
# convergence, standing survival, push recovery, walking speed tracking and
# controlled falls — the acceptance suite of docs/LOCOMOTION.md. Headless.
if(RX_BUILD_TESTS AND RX_JOLT AND "locomotion" IN_LIST RX_MODULES)
  add_executable(locomotion_test test/locomotion_test.cc)
  target_link_libraries(locomotion_test PRIVATE rx::locomotion rx::physics rx::core)
  add_test(NAME locomotion_test COMMAND locomotion_test)
endif()

# rx::character jetpack over the character controller's velocity seam: full-burn
# climb, fuel burn to empty + dead-stick fall, grounded-only refuel, spool lag to
# 90%, airborne lateral thrust-vector authority and NaN-free / jump-still-works.
# Headless (no GPU), needs Jolt.
if(RX_BUILD_TESTS AND RX_JOLT AND "character" IN_LIST RX_MODULES)
  add_executable(jetpack_test test/jetpack_test.cc)
  target_link_libraries(jetpack_test PRIVATE rx::character rx::physics rx::scene rx::ecs rx::core)
  add_test(NAME jetpack_test COMMAND jetpack_test)
endif()

# install()/export rules, generated last so every module target (and the
# RX_INSTALL_* feature flags the modules set) already exists.
if(RX_INSTALL)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/install.cmake)
endif()
