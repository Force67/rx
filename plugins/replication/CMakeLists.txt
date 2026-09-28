# rx::replication: streaming bubbles (interest management + ownership), entity
# replication and the client/server sessions, over rx::net's protocol. The
# sessions ride zetanet and compile only when rx::net was built with it.
set(RX_REPLICATION_SOURCES
  bubble.cc
  replication.cc
)
if(TARGET zetanet)
  list(APPEND RX_REPLICATION_SOURCES session.cc)
endif()

rx_add_module(replication ${RX_REPLICATION_SOURCES})
target_link_libraries(rx_replication PUBLIC rx::net rx::foundation rx::ecs rx::asset rx::scene)

# The 3D bubble visualizer is its own target so a headless dedicated server
# links rx::replication without dragging the renderer in.
if(TARGET rx_render)
  rx_add_module(net_viz bubble_debug.cc)
  target_link_libraries(rx_net_viz PUBLIC rx::replication rx::render)
  rx_embed_shaders(rx_net_viz
    shaders/bubble_wire.vs.hlsl
    shaders/bubble_wire.ps.hlsl)
endif()

if(RX_BUILD_TESTS AND "net" IN_LIST RX_MODULES)
  # rx::net / rx::replication unit tests: pure logic (wire codec, per-peer delta streams, bubble
  # membership + ownership), no GPU, driver or transport needed, so plain
  # `ctest` runs them everywhere.
  add_executable(net_test net_test.cc)
  target_link_libraries(net_test PRIVATE rx::replication rx::net rx::foundation)
  add_test(NAME net_test COMMAND net_test)
endif()
