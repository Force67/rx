# install()/export support: lets an out-of-tree game build+install rx once and
# consume it with find_package(rx) instead of add_subdirectory. Only pulled in
# when RX_INSTALL is ON (default: rx is the top-level project); an
# add_subdirectory consumer (e.g. recreation) never reaches this file, so its
# build is bit-identical to before.
#
# Package layout produced under <prefix>:
#   include/                    rx public headers, module-qualified
#                               (core/log.h, render/core/renderer.h, ...)
#   include/rx-deps/            bundled third-party public headers
#     equilibrium/base/...        equilibrium::base
#     vulkan/{vulkan,vk_video}    Vulkan-Headers (for volk / VMA / interop)
#     volk/volk.h                 volk loader
#     vma/vk_mem_alloc.h          VulkanMemoryAllocator
#   lib/                        rx_* static archives
#   lib/rx/                     bundled third-party static archives
#   lib/cmake/rx/               rxConfig / rxConfigVersion / rxTargets
#
# rx's own module archives are exported through install(EXPORT) with the rx::
# namespace. The third-party closure (equilibrium, volk, VMA, kinema, Jolt and
# the optional GPU SDKs) can not go through the same export cleanly: several are
# FetchContent'd / submodule / vendored targets whose include dirs point into
# read-only source trees, so install(EXPORT) rejects them. Static archives do
# NOT absorb their dependencies, so the consumer's linker still needs every one
# of those archives. The pragmatic, honest route (documented in EMBEDDING.md):
# bundle the archives + headers into the package and recreate them as IMPORTED
# targets in rxConfig.cmake, then re-attach them to the rx:: targets. The module
# CMakeLists wrap those references in $<BUILD_INTERFACE:...> so they drop out of
# the exported interface and rxConfig owns them for installed consumers.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(RX_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/rx)
set(RX_DEPS_LIBDIR ${CMAKE_INSTALL_LIBDIR}/rx)
set(RX_DEPS_INCDIR ${CMAKE_INSTALL_INCLUDEDIR}/rx-deps)

# --- rx module targets + export set -----------------------------------------
set(RX_MODULE_NAMES core ecs asset scene terrain render physics locomotion anim audio rpc
    character inventory inventory_world app)
set(RX_INSTALL_TARGETS)
foreach(_m ${RX_MODULE_NAMES})
  # Some modules are conditional (inventory_world only when rx_physics exists);
  # skip any whose target a given slice did not build so install() never
  # references a missing target.
  if(NOT TARGET rx_${_m})
    continue()
  endif()
  list(APPEND RX_INSTALL_TARGETS rx_${_m})
  # Export as rx::<m> (matching the in-tree rx::<m> alias) rather than the
  # default rx::rx_<m>.
  set_target_properties(rx_${_m} PROPERTIES EXPORT_NAME ${_m})
endforeach()

install(TARGETS ${RX_INSTALL_TARGETS}
  EXPORT rxTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT rxTargets
  FILE rxTargets.cmake
  NAMESPACE rx::
  DESTINATION ${RX_INSTALL_CMAKEDIR})

# --- rx public headers (module-qualified layout preserved) ------------------
install(DIRECTORY ${PROJECT_SOURCE_DIR}/engine/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING
    PATTERN "*.h"
    PATTERN "*.hpp"
    PATTERN "*.inl"
    # *_internal.h are kinema-visible glue (e.g. engine/anim/anim_internal.h),
    # not part of the public package: kinema headers are not bundled, so a public
    # header may never include <kinema/kinema.h>.
    PATTERN "*_internal.h" EXCLUDE)

# --- third-party archive + header bundling ----------------------------------
# Resolve an imported/interface target's own include dir(s), stripping the
# BUILD_INTERFACE genex wrapper and dropping any INSTALL_INTERFACE entry.
function(_rx_iface_includes out target)
  set(clean "")
  if(TARGET ${target})
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
      foreach(d ${dirs})
        string(REGEX REPLACE "\\$<BUILD_INTERFACE:([^>]+)>" "\\1" d "${d}")
        if(NOT d MATCHES "INSTALL_INTERFACE")
          list(APPEND clean "${d}")
        endif()
      endforeach()
    endif()
  endif()
  set(${out} "${clean}" PARENT_SCOPE)
endfunction()

# Install a static archive (real or imported target) under lib/rx.
function(_rx_bundle_archive target)
  if(TARGET ${target})
    install(FILES $<TARGET_FILE:${target}> DESTINATION ${RX_DEPS_LIBDIR})
  endif()
endfunction()

# equilibrium::base (PUBLIC: base:: types appear in rx headers) -> archive+headers
_rx_iface_includes(_eq_inc equilibrium::base)
list(GET _eq_inc 0 _eq_root)
_rx_bundle_archive(eq_base)
install(DIRECTORY ${_eq_root}/base
  DESTINATION ${RX_DEPS_INCDIR}/equilibrium
  FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.inl")

# volk + Vulkan-Headers + VMA (PUBLIC: reachable through rhi/vulkan_interop.h)
_rx_bundle_archive(volk)
get_target_property(_volk_src volk SOURCE_DIR)
install(FILES ${_volk_src}/volk.h ${_volk_src}/volk.c DESTINATION ${RX_DEPS_INCDIR}/volk)

_rx_iface_includes(_vk_inc Vulkan::Headers)
list(GET _vk_inc 0 _vk_root)
install(DIRECTORY ${_vk_root}/vulkan ${_vk_root}/vk_video
  DESTINATION ${RX_DEPS_INCDIR})

_rx_iface_includes(_vma_inc GPUOpen::VulkanMemoryAllocator)
list(GET _vma_inc 0 _vma_root)
install(FILES ${_vma_root}/vk_mem_alloc.h DESTINATION ${RX_DEPS_INCDIR}/vma)

# PRIVATE static deps: consumers need the archive at final link, never headers.
_rx_bundle_archive(kinema)
if(RX_INSTALL_JOLT)
  _rx_bundle_archive(Jolt)
endif()
if(RX_INSTALL_FSR3)
  _rx_bundle_archive(rx_ffx_fsr3)
endif()
if(RX_INSTALL_DLSS)
  _rx_bundle_archive(rx_dlss)
endif()
if(RX_INSTALL_NRD)
  _rx_bundle_archive(NRD)
  _rx_bundle_archive(ShaderMakeBlob)  # NRD links it for the embedded SPIR-V blobs
endif()

# --- config files -----------------------------------------------------------
if(SDL3_FOUND)
  set(RX_INSTALL_SDL3 ON)
endif()

# Normalize the feature flags (set as ON by the module CMakeLists only when the
# feature is built) so the generated rxConfig.cmake always sees ON/OFF.
foreach(_flag FSR3 DLSS NRD JOLT RHI_D3D12 WAYLAND_KDE_HDR SDL3)
  if(NOT DEFINED RX_INSTALL_${_flag})
    set(RX_INSTALL_${_flag} OFF)
  endif()
endforeach()

configure_package_config_file(
  ${PROJECT_SOURCE_DIR}/cmake/rxConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/rxConfig.cmake
  INSTALL_DESTINATION ${RX_INSTALL_CMAKEDIR}
  PATH_VARS RX_DEPS_LIBDIR RX_DEPS_INCDIR)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/rxConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/rxConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/rxConfigVersion.cmake
  DESTINATION ${RX_INSTALL_CMAKEDIR})
