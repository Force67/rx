# rx::nav -- tiled cost-aware navmesh, capped A* with entry costs, per-frame
# funnel steering, event-based repathing and terrain-aware position queries.
# Headless by design: tiles are sampled through a game callback, no physics or
# renderer dependency.
rx_add_module(nav
  navmesh.cc
  path.cc
  query.cc
  agent.cc
)
target_link_libraries(rx_nav PUBLIC rx::core rx::ecs rx::scene)

# Debug-line builders live in their own target so a dedicated server links
# rx::nav without dragging the renderer in (same split as rx::net_viz).
if(TARGET rx_render)
  rx_add_module(nav_viz nav_debug.cc)
  target_link_libraries(rx_nav_viz PUBLIC rx::nav rx::render)
endif()
