# The composition root a game embeds instead of forking the viewer: subsystem
# bringup/teardown, the fixed-step loop and FrameView assembly, driving the
# game through the app::Application callbacks. The one module allowed to link
# every subsystem; nothing below links back to it.
rx_add_module(app
  host.cc
)

target_link_libraries(rx_app PUBLIC
  rx::core rx::ecs rx::asset rx::scene rx::render rx::physics rx::audio)
