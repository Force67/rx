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
# https in rx::http. Off leaves the plain-http client, and an https URL then
# fails with a message rather than downgrading. See rxe/net/http/CMakeLists.txt.
option(RX_HTTPS "Build rx::http's https path (needs an mbedTLS checkout)" ON)
option(RX_USD "Enable OpenUSD scene loading (needs tools/get_tinyusdz.sh)" ON)
# Vulkan 1.1 accepts SPIR-V 1.3, or 1.4 with VK_KHR_spirv_1_4; the default
# shader target emits 1.6, which a conformant 1.1 driver rejects outright (only
# NVIDIA's is lenient enough to load it anyway). Turning this on retargets both
# shader toolchains so the modules load on a real 1.1 device. Off by default:
# the desktop build stays on the newer target env.
option(RX_SPIRV_1_4 "Emit SPIR-V 1.4 modules (loadable on Vulkan 1.1)" OFF)
option(RX_BUILD_TESTS "Build library test suites" ON)
option(RX_BUILD_RUNTIME "Build the rx viewer executable" ON)
option(RX_BUILD_EDITOR "Build the rx editor application (apps/editor)" OFF)
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
# is untouched. See cmake/install.cmake.
option(RX_INSTALL "Generate install()/export rules for find_package(rx)"
  ${PROJECT_IS_TOP_LEVEL})

# The "DLL test": build each rx module as a shared library instead of a static
# archive. Off by default (the static build is the shipping configuration). This
# is a correctness discipline, not a distribution mode: hidden visibility plus
# explicit per-module export annotations (foundation/build_config/export.h) document each
# module's real cross-DSO API and flush out ambient-state bugs.
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
    "each option-owning DSO re-applies env overrides at init. "
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
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/content.cmake)

find_package(SDL3 QUIET)
# An embedding project may have fetched SDL3 itself before adding rx (recreation
# does when the platform has no system SDL3, which is every cross build). The
# target is then already defined and find_package cannot see it, so reuse it
# rather than fetching a second, conflicting copy.
if(NOT SDL3_FOUND AND TARGET SDL3::SDL3)
  set(SDL3_FOUND TRUE)
endif()
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
  # base::Atomic on its native backend, not std::atomic. Forced so a build
  # directory configured when rx still needed the std mode (for zetanet's
  # std::memory_order arguments) does not keep it from its cache.
  set(EQ_USE_STD_ATOMIC OFF CACHE BOOL "" FORCE)
  set(EQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/equilibrium equilibrium EXCLUDE_FROM_ALL)
  # base's std::formatter specializations exist for consumers that format base
  # strings with <format>; nothing here does, and they drag <format> into every
  # TU that names a base::String.
  target_compile_definitions(eq_base PUBLIC BASE_NO_STD_FORMAT)
endif()

add_subdirectory(third_party)

# Overridable so special-purpose builds (e.g. the mingw cross-compile that
# packages the d3d12 backend tests for a Windows runtime) can build a slice.
if(NOT DEFINED RX_MODULES)
  set(RX_MODULES
    ui/events
    ui/window
    gpu
    ecs
    script
    asset
    importers
    scene
    terrain
    edit
    world
    render
    ui/imgui
    render2d
    weather
    placement
    physics
    anim
    audio
    vehicles
    net/rpc
    devtools
    net/transport
    replication
    net/http
    nav
    character
    combat
    inventory
    locomotion
    ui/ugui
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
# when the target is absent. KINEMA_USE_BASE is kinema's equilibrium
# configuration: no STL, no exceptions, base types across its API, built on the
# equilibrium::base target vendored above.
if(NOT TARGET kinema)
  set(RX_KINEMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../kinema" CACHE PATH "Path to a kinema checkout")
  if(NOT EXISTS "${RX_KINEMA_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "kinema not found at ${RX_KINEMA_DIR}; clone https://github.com/Force67/kinema next to rx (or set RX_KINEMA_DIR)")
  endif()
  set(KINEMA_USE_BASE ON CACHE BOOL "" FORCE)
  set(KINEMA_BUILD_TESTS ${RX_BUILD_TESTS})
  add_subdirectory(${RX_KINEMA_DIR} kinema)
endif()

# zetanet: the UDP transport rx::replication's sessions and rx::net's rpc channel
# ride, consumed as an optional sibling checkout. A parent project may already
# provide the target; without either, both still build their transport-free
# halves (codec, replication, interest bubbles) and only the sessions and the
# rpc channel are absent. ZNET_USE_STL=OFF is
# zetanet's equilibrium configuration: it builds on base alone, without the STL
# or exceptions, and reuses the equilibrium::base target vendored above rather
# than compiling its own copy.
if(NOT TARGET zetanet AND "net/transport" IN_LIST RX_MODULES)
  set(RX_ZETANET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zetanet" CACHE PATH
    "Path to a zetanet checkout (develop branch); missing disables the net sessions")
  if(EXISTS "${RX_ZETANET_DIR}/CMakeLists.txt")
    set(ZNET_USE_STL OFF CACHE BOOL "" FORCE)
    set(ZNET_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RX_ZETANET_DIR} zetanet EXCLUDE_FROM_ALL)
  else()
    message(STATUS "rx: no zetanet at ${RX_ZETANET_DIR}, net sessions disabled")
  endif()
endif()

# mbedTLS backs rx::http's https path, and is the only external dependency that
# client has. Nothing new is fetched: equilibrium vendors mbedTLS as a submodule
# and zetanet vendors its own, so whichever checkout is present is reused.
# A parent project that already builds an `mbedtls` target (recreation, when
# zetanet takes its mbedTLS crypto backend) wins outright - two copies of it in
# one link is a symbol clash, and the client compiles against either major
# version (see the MBEDTLS_VERSION_MAJOR branches in tls_stream.cc).
# NOT TARGET mbedtls is not enough on its own: zetanet finds its mbedTLS with
# find_library, which leaves a PATH in MBEDTLS_LIB and no target at all. Adding
# a second copy on top of that puts two mbedTLS builds in one link with
# different mbedtls_ssl_context layouts, and the linker picks one set of
# function bodies for structs laid out by the other.
if(RX_HTTPS AND "net/http" IN_LIST RX_MODULES AND NOT TARGET mbedtls AND NOT MBEDTLS_LIB)
  set(RX_MBEDTLS_DIR "" CACHE PATH "Path to an mbedTLS checkout for rx::http's https")
  if(NOT RX_MBEDTLS_DIR)
    foreach(_candidate
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/equilibrium/external/mbedtls"
        "${RX_ZETANET_DIR}/vendor/mbedtls")
      if(EXISTS "${_candidate}/CMakeLists.txt")
        set(RX_MBEDTLS_DIR "${_candidate}" CACHE PATH "" FORCE)
        break()
      endif()
    endforeach()
  endif()
  if(EXISTS "${RX_MBEDTLS_DIR}/CMakeLists.txt")
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
    # A vendored release we do not get to edit, built with -Werror: every new
    # compiler finds something new in it, and none of it is ours to fix.
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${RX_MBEDTLS_DIR} mbedtls EXCLUDE_FROM_ALL)
    # mbedTLS 3.x ships with TLS 1.3 compiled out. rx::http talks to the open
    # web, where 1.3 is what a modern endpoint negotiates and a few already
    # refuse anything older, so it is switched on here rather than left as a
    # surprise. PUBLIC on every library: the option changes the layout of
    # mbedtls_ssl_context, so the library and everyone including its headers
    # have to be compiled with the same answer. Harmless on 4.x, which defines
    # the same symbol itself.
    # COMPATIBILITY_MODE is not optional in practice: a TLS 1.3 server sends a
    # dummy ChangeCipherSpec record so middleboxes keep believing they are
    # watching TLS 1.2, and without this mbedTLS calls that record invalid and
    # fails every handshake on the open web. Verified: with 1.3 on and this off,
    # example.com, google.com and cloudflare.com all died at EncryptedExtensions.
    foreach(_mbed mbedtls mbedx509 mbedcrypto)
      if(TARGET ${_mbed})
        target_compile_definitions(${_mbed} PUBLIC
          MBEDTLS_SSL_PROTO_TLS1_3
          MBEDTLS_SSL_TLS1_3_COMPATIBILITY_MODE)
        # http.h tells callers to Fetch from a worker, and TLS 1.3 is PSA-only:
        # every handshake touches mbedTLS's global key-slot table and RNG. That
        # table grew its own locking in 3.6, so on 3.5 the locks have to be
        # switched on here or two concurrent https requests race.
        if(NOT WIN32)
          find_package(Threads REQUIRED)
          target_compile_definitions(${_mbed} PUBLIC
            MBEDTLS_THREADING_C
            MBEDTLS_THREADING_PTHREAD)
          target_link_libraries(${_mbed} PUBLIC Threads::Threads)
        endif()
      endif()
    endforeach()
  else()
    message(STATUS
      "rx: no mbedtls checkout found (set RX_MBEDTLS_DIR), rx::http is http-only")
  endif()
endif()

# A downstream fork registers its own rxe/<name> or plugins/<name> modules by
# shipping this file: it runs with RX_MODULES already populated, so it appends to
# that list (and sets whatever paths its modules need) without editing anything
# upstream owns. Nothing in this tree provides one, which is the point - a merge
# from here can never touch it, so a fork carrying private modules stays
# mergeable with no conflict in the shared build files.
include("${CMAKE_CURRENT_SOURCE_DIR}/rxe/local.cmake" OPTIONAL)

# RX_MODULES names modules, not paths: each lives in rxe/ or, if it is optional
# per game, in plugins/ (docs/STRUCTURE.md).
add_subdirectory(foundation)
foreach(module ${RX_MODULES})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/rxe/${module}/CMakeLists.txt)
    add_subdirectory(rxe/${module})
  elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/${module}/CMakeLists.txt)
    add_subdirectory(plugins/${module})
  else()
    message(FATAL_ERROR "RX_MODULES: no module '${module}' in rxe/ or plugins/")
  endif()
endforeach()

if(RX_BUILD_EXAMPLES AND "anim" IN_LIST RX_MODULES)
  add_subdirectory(examples)
endif()

if(RX_BUILD_RUNTIME)
  add_subdirectory(runtime)
endif()

# The editor application: an app::Application whose entire UI is rx::ui.
if(RX_BUILD_EDITOR)
  add_subdirectory(apps/editor)
endif()

# Asset authoring tools. rxpack creates/lists/extracts/verifies .rxp game
# archives (rxe/asset/pack.h).
if(RX_BUILD_TOOLS)
  add_executable(rxpack tools/rxpack.cc)
  target_link_libraries(rxpack PRIVATE rx::asset rx::foundation)
  rx_set_warnings(rxpack)
endif()

# Packing an .rxp happens at build time, on the *host*, so a cross build cannot
# use the rxpack it just produced for the target. RX_RXPACK names a host-built
# one instead (the cross build scripts configure a small native tree to get it).
# Cached INTERNAL so both rx's own archive rules and an embedding project's see
# the same answer. Empty means: pack nothing, and everything that reads an
# archive degrades to loose files.
set(RX_RXPACK "" CACHE FILEPATH
  "Host rxpack executable, for cross builds (a native build uses its own)")
if(NOT CMAKE_CROSSCOMPILING AND TARGET rxpack)
  set(RX_RXPACK_COMMAND "$<TARGET_FILE:rxpack>" CACHE INTERNAL "rxpack invocation")
  set(RX_RXPACK_DEPENDS rxpack CACHE INTERNAL "rxpack build dependency")
elseif(RX_RXPACK)
  set(RX_RXPACK_COMMAND "${RX_RXPACK}" CACHE INTERNAL "rxpack invocation")
  set(RX_RXPACK_DEPENDS "" CACHE INTERNAL "rxpack build dependency")
else()
  set(RX_RXPACK_COMMAND "" CACHE INTERNAL "rxpack invocation")
  set(RX_RXPACK_DEPENDS "" CACHE INTERNAL "rxpack build dependency")
  message(STATUS "no host rxpack (set RX_RXPACK); engine archives will not be packed")
endif()

# rxworld cooks an authored .rxscene into a baked, streamable world archive
# (rxe/world/): a world index plus one payload per cell and domain, packed
# into one .rxp the engine mounts like any other content.
if(RX_BUILD_TOOLS AND "world" IN_LIST RX_MODULES)
  add_executable(rxworld tools/rxworld.cc)
  target_link_libraries(rxworld PRIVATE rx::world rx::edit rx::asset rx::ecs rx::foundation)
  rx_set_warnings(rxworld)
endif()

# Cooking a world happens at build time, on the *host*, so a cross build cannot
# use the rxworld it just produced for the target. The same problem RX_RXPACK
# solves above, and the same shape of answer: RX_RXWORLD names a host-built one,
# and empty means no world is cooked - `rx --world` then has nothing to open,
# which is a missing test map rather than a broken build.
set(RX_RXWORLD "" CACHE FILEPATH
  "Host rxworld executable, for cross builds (a native build uses its own)")
if(NOT CMAKE_CROSSCOMPILING AND TARGET rxworld)
  set(RX_RXWORLD_COMMAND "$<TARGET_FILE:rxworld>")
  set(RX_RXWORLD_DEPENDS rxworld)
elseif(RX_RXWORLD)
  set(RX_RXWORLD_COMMAND "${RX_RXWORLD}")
  set(RX_RXWORLD_DEPENDS "")
else()
  set(RX_RXWORLD_COMMAND "")
  set(RX_RXWORLD_DEPENDS "")
endif()

# The baked world `rx --world` streams. Cooked at build time rather than checked
# in as a binary: the .rxscene beside it is the authored source, and running the
# real cook every build is also the only thing that would notice the tool and
# the runtime drifting apart. Lives here rather than in runtime/CMakeLists.txt
# because that is added before rxworld exists.
if(TARGET rx AND RX_RXWORLD_COMMAND)
  set(RX_STREAMWORLD_SCENE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/scenes/streamworld.rxscene)
  # A plain path rather than $<TARGET_FILE_DIR:rx>: a generator expression in an
  # OUTPUT cannot name a target from another directory, and rx is added from
  # runtime/.
  set(RX_STREAMWORLD_DIR ${CMAKE_BINARY_DIR}/runtime/worlds)
  # No --name: the cook takes the archive's filename stem, so the archive, the
  # world inside it, and a bare `rx --world` all agree without being told.
  set(RX_STREAMWORLD_ARCHIVE ${RX_STREAMWORLD_DIR}/streamworld.rxp)
  add_custom_command(
    OUTPUT ${RX_STREAMWORLD_ARCHIVE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${RX_STREAMWORLD_DIR}
    COMMAND ${RX_RXWORLD_COMMAND} bake ${RX_STREAMWORLD_SCENE} ${RX_STREAMWORLD_ARCHIVE}
            --cell-size 32
    DEPENDS ${RX_RXWORLD_DEPENDS} ${RX_STREAMWORLD_SCENE}
    COMMENT "Cooking the streamworld test map")
  add_custom_target(rx-streamworld ALL DEPENDS ${RX_STREAMWORLD_ARCHIVE})
  add_dependencies(rx rx-streamworld)
elseif(TARGET rx)
  message(STATUS "no host rxworld (set RX_RXWORLD); the streamworld test map will not be cooked")
endif()

# rxdiff compares two rendered pngs against a measured tolerance, which is the
# only comparison a renderer whose radiance cache races its own waves admits
# (a capture's clock is locked, so nothing else about it wanders). Its own
# tool rather than an rx flag: it needs no device, no window and no scene, so it
# links stb and nothing else and starts instantly in a CI loop.
if(RX_BUILD_TOOLS)
  add_executable(rxdiff tools/rxdiff.cc)
  target_link_libraries(rxdiff PRIVATE rx::stb_impl equilibrium::base)
  rx_set_warnings(rxdiff)
endif()

# rxcall issues one command to a running engine's --authoring-endpoint socket.
# It links rx::rpc rather than reimplementing the frame, so the client and the
# engine can never disagree about the encoding. Unix sockets only, like the
# endpoint itself.
if(RX_BUILD_TOOLS AND NOT WIN32 AND "net/rpc" IN_LIST RX_MODULES)
  add_executable(rxcall tools/rxcall.cc)
  target_link_libraries(rxcall PRIVATE rx::rpc rx::foundation)
  rx_set_warnings(rxcall)
endif()

# Everything the engine ships, packed into one archive: rxe/resources (fonts/
# and the platform tiers in config/), as rxe://fonts/ and rxe://config/. Games
# add their own archives beside it (docs/CONFIG.md).
if(RX_RXPACK_COMMAND)
  rx_add_archive(rx_engine_archives OUTPUT ${PROJECT_BINARY_DIR}/Data/rx_engine.rxp
    CONTENT fonts ${CMAKE_CURRENT_SOURCE_DIR}/rxe/resources/fonts
            config ${CMAKE_CURRENT_SOURCE_DIR}/rxe/resources/config)
  rx_finish_content_staging()
endif()

# Exceptions are not used anywhere in rx: every target defined in rx's own
# directories builds without them, so a throw, try or catch fails to compile
# instead of creeping back in. Third-party directories keep their own flags.
function(rx_forbid_exceptions dir)
  get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target ${targets})
    get_target_property(type ${target} TYPE)
    if(type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
      if(MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
      else()
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
      endif()
    endif()
  endforeach()
  get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(subdir ${subdirs})
    cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${subdir}" NORMALIZE inside)
    string(FIND "${subdir}" "${PROJECT_SOURCE_DIR}/third_party" third_party_at)
    if(inside AND NOT third_party_at EQUAL 0)
      rx_forbid_exceptions(${subdir})
    endif()
  endforeach()
endfunction()
rx_forbid_exceptions(${CMAKE_CURRENT_SOURCE_DIR})

# The include lint (docs/STRUCTURE.md): every target's includes must resolve to
# itself or to a target it links, plus the layering rules on top.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/module_graph.cmake)
rx_write_module_graph()
if(RX_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME checkincludes COMMAND Python3::Interpreter
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/checkincludes/checkincludes.py
      ${CMAKE_BINARY_DIR}/modules.json)
  endif()
endif()

# install()/export rules, generated last so every module target (and the
# RX_INSTALL_* feature flags the modules set) already exists.
if(RX_INSTALL)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/install.cmake)
endif()
