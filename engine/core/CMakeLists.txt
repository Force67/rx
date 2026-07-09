rx_add_module(core
  log.cc
  job_system.cc
  frame_timer.cc
  world_clock.cc
  window.cc
  input_bindings.cc
  feature_registry.cc
)

# equilibrium's base library; linking it here makes base:: containers and
# strings available to every engine module.
target_link_libraries(rx_core PUBLIC
  equilibrium::base
  $<$<PLATFORM_ID:Linux>:pthread>)

if(SDL3_FOUND)
  target_sources(rx_core PRIVATE window_sdl3.cc)
  target_link_libraries(rx_core PUBLIC SDL3::SDL3)
  target_compile_definitions(rx_core PRIVATE RX_HAS_SDL3=1)

  # KWin's per-output HDR toggle (kde_output_device_v2): the reliable "is HDR
  # enabled" signal on KDE; SDL's property only reflects luminance headroom.
  # Optional: skipped when wayland-client is not around (windows/macos/x11-only
  # distros), where hdr_enabled falls back to the SDL property.
  if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    endif()
    if(WAYLAND_CLIENT_FOUND)
      target_sources(rx_core PRIVATE
        wayland_kde_hdr.cc
        ${PROJECT_SOURCE_DIR}/third_party/plasma-wayland/kde-output-device-v2-protocol.c)
      target_include_directories(rx_core PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/plasma-wayland)
      target_link_libraries(rx_core PRIVATE PkgConfig::WAYLAND_CLIENT)
      target_compile_definitions(rx_core PRIVATE RX_HAS_WAYLAND_KDE_HDR=1)
    endif()
  endif()
endif()

if(ANDROID)
  target_sources(rx_core PRIVATE window_android.cc)
  target_link_libraries(rx_core PUBLIC volk log android)
endif()
