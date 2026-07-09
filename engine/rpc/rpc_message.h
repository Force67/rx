#ifndef RX_RPC_RPC_MESSAGE_H_
#define RX_RPC_RPC_MESSAGE_H_

#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "rpc/rpc_value.h"

namespace rx::rpc {

// A single remote procedure call: the name of the handler to invoke and the
// argument list. This is the in-memory form; EncodeCall/DecodeCall translate it
// to and from the compact wire bytes that the separate net layer transports.
struct RpcCall {
  std::string name;
  RpcArgs args;
};

// Serializes a call to the little-endian wire form (see rpc_message.cc for the
// exact byte layout). Always succeeds for in-memory calls under the wire limits.
std::vector<u8> EncodeCall(const RpcCall& call);

// Decodes a call from a raw byte buffer that arrived over the network. The input
// is treated as hostile: every field is bounds-checked, the structural limits
// are enforced, and any malformed, truncated, oversized, or trailing-garbage
// input yields std::nullopt rather than a partial or out-of-range result.
std::optional<RpcCall> DecodeCall(const u8* data, size_t size);

}  // namespace rx::rpc

#endif  // RX_RPC_RPC_MESSAGE_H_
