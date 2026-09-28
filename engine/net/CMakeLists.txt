# rx::net: the wire protocol and, when the transport is present, the rpc channel.
# rx::replication: streaming bubbles (interest management + ownership), entity
# replication and the client/server sessions. Replication knows the entity
# world; net does not, so a tool that only speaks the protocol links no ECS.
#
# The protocol, codec and interest code have no transport dependency and always
# build; the sessions and the rpc channel ride zetanet and compile only when the
# parent (or the optional sibling checkout, see the top CMakeLists) provides the
# `zetanet` target. RX_NET_HAS_ZETANET is a PUBLIC define so consumers can tell
# which flavor they linked.
set(RX_NET_SOURCES protocol.cc)
set(RX_REPLICATION_SOURCES
  bubble.cc
  replication.cc
)
if(TARGET zetanet)
  list(APPEND RX_NET_SOURCES rpc_channel.cc)
  list(APPEND RX_REPLICATION_SOURCES session.cc)
endif()

rx_add_module(net ${RX_NET_SOURCES})
target_link_libraries(rx_net PUBLIC rx::core rx::rpc)
if(TARGET zetanet)
  target_link_libraries(rx_net PUBLIC zetanet)
  target_compile_definitions(rx_net PUBLIC RX_NET_HAS_ZETANET=1)
endif()

rx_add_module(replication ${RX_REPLICATION_SOURCES})
target_link_libraries(rx_replication PUBLIC rx::net rx::core rx::ecs rx::asset rx::scene)

# The 3D bubble visualizer is its own target so a headless dedicated server
# links rx::replication without dragging the renderer in.
if(TARGET rx_render)
  rx_add_module(net_viz bubble_debug.cc)
  target_link_libraries(rx_net_viz PUBLIC rx::replication rx::render)
  rx_embed_shaders(rx_net_viz
    shaders/bubble_wire.vs.hlsl
    shaders/bubble_wire.ps.hlsl)
endif()
