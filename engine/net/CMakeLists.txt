# rx::net -- replication, streaming bubbles (interest management + ownership)
# and the client/server sessions.
#
# The codec + interest half has no transport dependency and always builds; the
# sessions and the rpc channel ride zetanet and compile only when the parent
# (or the optional sibling checkout, see the top CMakeLists) provides the
# `zetanet` target. RX_NET_HAS_ZETANET is a PUBLIC define so consumers can
# tell which flavor they linked.
set(RX_NET_SOURCES
  bubble.cc
  protocol.cc
  replication.cc
)
if(TARGET zetanet)
  list(APPEND RX_NET_SOURCES
    rpc_channel.cc
    session.cc
  )
endif()

rx_add_module(net ${RX_NET_SOURCES})
target_link_libraries(rx_net PUBLIC rx::core rx::ecs rx::asset rx::scene rx::rpc)
if(TARGET zetanet)
  target_link_libraries(rx_net PUBLIC zetanet)
  target_compile_definitions(rx_net PUBLIC RX_NET_HAS_ZETANET=1)
endif()

# The 3D bubble visualizer is its own target so a headless dedicated server
# links rx::net without dragging the renderer in.
if(TARGET rx_render)
  rx_add_module(net_viz bubble_debug.cc)
  target_link_libraries(rx_net_viz PUBLIC rx::net rx::render)
  rx_embed_shaders(rx_net_viz
    shaders/bubble_wire.vs.hlsl
    shaders/bubble_wire.ps.hlsl)
endif()
