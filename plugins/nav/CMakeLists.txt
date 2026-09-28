# rx::nav: tiled cost-aware navmesh, capped A* with entry costs, per-frame
# funnel steering, event-based repathing and terrain-aware position queries.
# Headless by design: tiles are sampled through a game callback, no physics or
# renderer dependency.
rx_add_module(nav
  navmesh.cc
  path.cc
  query.cc
  agent.cc
)
target_link_libraries(rx_nav PUBLIC rx::foundation rx::ecs rx::scene)

# Debug-line builders live in their own target so a dedicated server links
# rx::nav without dragging the renderer in (same split as rx::net_viz).
if(TARGET rx_render)
  rx_add_module(nav_viz nav_debug.cc)
  target_link_libraries(rx_nav_viz PUBLIC rx::nav rx::render)
endif()

if(RX_BUILD_TESTS)
  # rx::nav unit tests: pure CPU logic over a synthetic terrain sampler (costed
  # A*, entry-cost commitment, funnel, event repathing, position scoring), so
  # plain `ctest` runs them everywhere.
  add_executable(nav_test nav_test.cc)
  target_link_libraries(nav_test PRIVATE rx::nav rx::scene rx::ecs rx::foundation)
  add_test(NAME nav_test COMMAND nav_test)
endif()
