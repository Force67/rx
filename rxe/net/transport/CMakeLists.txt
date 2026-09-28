# rx::net: the wire protocol and, when the transport is present, the rpc channel.
# It knows no entity world; entity replication and the sessions are the
# replication plugin (plugins/replication).
#
# The protocol and codec have no transport dependency and always build; the rpc
# channel rides zetanet and compiles only when the parent (or the optional
# sibling checkout, see the top CMakeLists) provides the `zetanet` target.
# RX_NET_HAS_ZETANET is a PUBLIC define so consumers can tell which flavor they
# linked.
set(RX_NET_SOURCES protocol.cc)
if(TARGET zetanet)
  list(APPEND RX_NET_SOURCES rpc_channel.cc)
endif()

rx_add_module(net ${RX_NET_SOURCES})
target_link_libraries(rx_net PUBLIC rx::foundation rx::rpc)
if(TARGET zetanet)
  target_link_libraries(rx_net PUBLIC zetanet)
  target_compile_definitions(rx_net PUBLIC RX_NET_HAS_ZETANET=1)
endif()
