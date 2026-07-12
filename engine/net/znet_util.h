#ifndef RX_NET_ZNET_UTIL_H_
#define RX_NET_ZNET_UTIL_H_

// Zetanet glue shared by the sessions and the rpc channel. Only compiled when
// the transport is available (RX_NET_HAS_ZETANET); the codec/interest halves
// of the module never include this.

#include <vector>

#include <znet/z_packets.h>
#include <znet/z_peer.h>

#include "core/types.h"
#include "net/protocol.h"

namespace rx::net {

inline tx::network::PacketType ToPacketType(u16 type) {
  return static_cast<tx::network::PacketType>(type);
}

// Wraps an encoded payload in an outgoing zetanet packet. Snapshots stay
// unreliable by design, joins and keyframes ride the reliable path.
inline tx::network::OutgoingPacket MakePacket(
    u32 destination, u16 type, const std::vector<u8>& payload, bool reliable,
    tx::network::PacketPriority priority = tx::network::PacketPriority::Medium) {
  const tx::network::PackageFlags flags{
      .reliable = reliable ? u8{1} : u8{0},
      .encrypted = 0,
      .compressed = 0,
      .priority = static_cast<u8>(priority),
      .acknowledged = 0,
      .awaiting_ack = reliable ? u8{1} : u8{0},
      .reserved = 0,
  };
  return tx::network::OutgoingPacket(
      destination, ToPacketType(type), tx::network::PacketChannelType::Data,
      flags,
      base::Span<byte>(reinterpret_cast<const byte*>(payload.data()),
                       payload.size()));
}

inline tx::network::OutgoingPacket MakePacket(
    u32 destination, MessageType type, const std::vector<u8>& payload, bool reliable,
    tx::network::PacketPriority priority = tx::network::PacketPriority::Medium) {
  return MakePacket(destination, static_cast<u16>(type), payload, reliable, priority);
}

inline const u8* PacketData(const tx::network::IncomingPacket& packet) {
  return reinterpret_cast<const u8*>(packet.data.data());
}

}  // namespace rx::net

#endif  // RX_NET_ZNET_UTIL_H_
