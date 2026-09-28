# rx::window: the OS window and its input pump, one backend per platform
# selected by the build (window_sdl3.cc, window_android.cc).
rx_add_module(window window.cc)
target_link_libraries(rx_window PUBLIC rx::events rx::foundation)

if(SDL3_FOUND)
  target_sources(rx_window PRIVATE window_sdl3.cc)
  target_link_libraries(rx_window PUBLIC SDL3::SDL3)
  target_compile_definitions(rx_window PRIVATE RX_HAS_SDL3=1)

  # KWin's per-output HDR toggle (kde_output_device_v2): the reliable "is HDR
  # enabled" signal on KDE; SDL's property only reflects luminance headroom.
  # Optional: skipped when wayland-client is not around (windows/macos/x11-only
  # distros), where hdr_enabled falls back to the SDL property.
  if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      # >= 1.20 for wl_proxy_marshal_flags, which the generated
      # kde_output_device_v2 stubs call. Older wayland-client (Steam Runtime 3
      # sniper ships 1.18) still satisfies a bare wayland-client and then fails
      # to compile, so the version floor belongs in the probe.
      pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client>=1.20)
    endif()
    if(WAYLAND_CLIENT_FOUND)
      target_sources(rx_window PRIVATE
        wayland_kde_hdr.cc
        ${PROJECT_SOURCE_DIR}/third_party/plasma-wayland/kde-output-device-v2-protocol.c)
      target_include_directories(rx_window PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/plasma-wayland)
      # BUILD_INTERFACE: kept out of the installed export; rxConfig.cmake
      # re-resolves wayland-client via pkg-config for find_package consumers.
      target_link_libraries(rx_window PRIVATE $<BUILD_INTERFACE:PkgConfig::WAYLAND_CLIENT>)
      set(RX_INSTALL_WAYLAND_KDE_HDR ON PARENT_SCOPE)
      target_compile_definitions(rx_window PRIVATE RX_HAS_WAYLAND_KDE_HDR=1)
    endif()
  endif()
endif()

if(ANDROID)
  target_sources(rx_window PRIVATE window_android.cc)
  target_link_libraries(rx_window PUBLIC volk android)
endif()
