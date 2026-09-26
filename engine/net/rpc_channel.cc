#include "net/rpc_channel.h"

#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "core/log.h"
#include "net/znet_util.h"

namespace rx::net {
namespace {

// An encoded call must fit in one datagram (no fragmentation). A call larger
// than this is refused rather than silently dropped on the wire.
constexpr size_t kMaxRpcPacket = 60000;

}  // namespace

void RpcServerChannel::OnPacket(u32 peer, const u8* data, size_t size) {
  base::Optional<rpc::RpcCall> call = rpc::DecodeCall(data, size);
  if (!call) {
    RX_WARN("net: dropped corrupt rpc from peer {}", peer);
    return;
  }
  const rpc::RpcContext ctx{peer, /*from_server=*/false};
  if (!registry_.Dispatch(ctx, *call)) {
    RX_WARN("net: no rpc handler '{}' (from peer {})", call->name, peer);
  }
}

bool RpcServerChannel::Send(u32 destination, const rpc::RpcCall& call) {
  base::Vector<u8> payload = rpc::EncodeCall(call);
  if (payload.size() > kMaxRpcPacket) {
    RX_WARN("net: rpc '{}' is too large to send ({} bytes)", call.name, payload.size());
    return false;
  }
  server_.Push(MakePacket(destination, MessageType::kRpcCall, payload,
                          /*reliable=*/true, tx::network::PacketPriority::Medium));
  return true;
}

bool RpcServerChannel::EmitToClient(u32 peer, const rpc::RpcCall& call) {
  return Send(peer, call);
}

bool RpcServerChannel::EmitToClient(u32 peer, base::String name, rpc::RpcArgs args) {
  return Send(peer, rpc::RpcCall{base::move(name), base::move(args)});
}

bool RpcServerChannel::Broadcast(const rpc::RpcCall& call) {
  return Send(tx::network::ZPeerId::to_all, call);
}

bool RpcServerChannel::Broadcast(base::String name, rpc::RpcArgs args) {
  return Send(tx::network::ZPeerId::to_all,
              rpc::RpcCall{base::move(name), base::move(args)});
}

void RpcClientChannel::OnPacket(const u8* data, size_t size) {
  base::Optional<rpc::RpcCall> call = rpc::DecodeCall(data, size);
  if (!call) {
    RX_WARN("net: dropped corrupt rpc from server");
    return;
  }
  const rpc::RpcContext ctx{0, /*from_server=*/true};
  if (!registry_.Dispatch(ctx, *call)) {
    RX_WARN("net: no rpc handler '{}' (from server)", call->name);
  }
}

bool RpcClientChannel::EmitToServer(const rpc::RpcCall& call) {
  base::Vector<u8> payload = rpc::EncodeCall(call);
  if (payload.size() > kMaxRpcPacket) {
    RX_WARN("net: rpc '{}' is too large to send ({} bytes)", call.name, payload.size());
    return false;
  }
  client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kRpcCall,
                          payload, /*reliable=*/true, tx::network::PacketPriority::Medium));
  return true;
}

bool RpcClientChannel::EmitToServer(base::String name, rpc::RpcArgs args) {
  return EmitToServer(rpc::RpcCall{base::move(name), base::move(args)});
}

}  // namespace rx::net
