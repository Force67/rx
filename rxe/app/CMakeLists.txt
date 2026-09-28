# The composition root a game embeds instead of forking the viewer: subsystem
# bringup/teardown, the fixed-step loop and FrameView assembly, driving the
# game through the app::Application callbacks. The one module allowed to link
# every subsystem; nothing below links back to it.
rx_add_module(app
  host.cc
  platform_config.cc
  world_clock.cc
)

target_link_libraries(rx_app PUBLIC
  rx::foundation rx::window rx::events rx::ecs rx::asset rx::scene rx::render rx::physics rx::audio
  # The splash plate the host puts up before the application sees a frame.
  rx::ui)

if(RX_BUILD_TESTS)
  # Platform config: include and section rules, every shipped tier in
  # rxe/resources/config resolving cleanly, and the android tier detection.
  add_executable(platform_config_test platform_config_test.cc)
  target_compile_definitions(platform_config_test PRIVATE
    RX_CONFIG_SOURCE_DIR="${PROJECT_SOURCE_DIR}/rxe/resources/config")
  target_link_libraries(platform_config_test PRIVATE rx::app)
  add_test(NAME platform_config_test COMMAND platform_config_test)
endif()
