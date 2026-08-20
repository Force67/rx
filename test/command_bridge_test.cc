// rx::authoring acceptance: the live command endpoint, from an encoded rpc frame
// on a unix socket all the way into a real ecs::World and back. Covers the two
// things that make it safe to leave in a shipping binary -- the signature check
// in front of every handler, and the trust gate in front of the registry -- with
// no window, GPU or transport, so plain ctest runs it.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "authoring/command_bridge.h"
#include "authoring/command_endpoint.h"
#include "ecs/world.h"
#include "rpc/rpc_message.h"
#include "scene/components.h"
#include "scene/scene_handlers.h"
#include "script/handler_context.h"
#include "script/handler_registry.h"
#include "script/script_arena.h"
#include "script/script_symbols.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

namespace authoring = rx::authoring;
namespace ecs = rx::ecs;
namespace rpc = rx::rpc;
namespace scene = rx::scene;
namespace script = rx::script;
using rx::u8;
using rx::u32;

// The same wiring the viewer does for --authoring-endpoint.
struct Rig {
  ecs::World world;
  script::ScriptSymbols symbols;
  script::ScriptArena scratch;
  script::HandlerRegistry commands;
  script::HandlerContext ctx;
  std::optional<authoring::CommandBridge> bridge;  // built after the registry is filled

  Rig() {
    scene::SetupSceneCommands(commands);
    ctx.world = &world;
    ctx.symbols = &symbols;
    ctx.scratch = &scratch;
    bridge.emplace(commands, ctx);
  }

  authoring::CommandBridge::Reply Call(std::string name, rpc::RpcArgs args) {
    const rpc::RpcContext local{authoring::kLocalSender, /*from_server=*/false};
    return bridge->Invoke(local, rpc::RpcCall{std::move(name), std::move(args)});
  }
};

void TestMarshalling() {
  Rig rig;
  CHECK(rig.bridge->registry().size() == rig.commands.size());
  CHECK(rig.bridge->registry().Has("World.Spawn"));

  // World.Spawn(symbol, vec3, float) -- 3 params, 5 wire args.
  authoring::CommandBridge::Reply reply =
      rig.Call("World.Spawn", {rpc::RpcValue(std::string("crate")), rpc::RpcValue(1.0),
                               rpc::RpcValue(2.0), rpc::RpcValue(3.0), rpc::RpcValue(rx::i64(2))});
  CHECK(reply.ok);
  CHECK(reply.values.size() == 1);
  const rx::i64 entity = reply.values[0].as_int();
  const ecs::Entity spawned{static_cast<u32>(static_cast<rx::u64>(entity)),
                            static_cast<u32>(static_cast<rx::u64>(entity) >> 32)};
  CHECK(rig.world.IsAlive(spawned));
  // The scale arg went in as an int where the signature wants a float: that
  // widening is the one conversion the bridge performs.
  CHECK(rig.world.Get<scene::Transform>(spawned)->scale == 2.0f);

  reply = rig.Call("World.Teleport", {rpc::RpcValue(entity), rpc::RpcValue(4.0),
                                      rpc::RpcValue(5.0), rpc::RpcValue(6.0)});
  CHECK(reply.ok && reply.values.empty());  // void returns nothing, not null

  reply = rig.Call("World.GetPosition", {rpc::RpcValue(entity)});
  CHECK(reply.ok && reply.values.size() == 3);  // a vec3 return is three floats
  CHECK(reply.values[0].as_float() == 4.0);
  CHECK(reply.values[2].as_float() == 6.0);

  // A string round trip: in as a borrowed view, out of the scratch arena and
  // copied into the reply, so resetting the arena cannot invalidate it.
  rig.Call("World.SetName", {rpc::RpcValue(entity), rpc::RpcValue(std::string("Crate 01"))});
  reply = rig.Call("World.GetName", {rpc::RpcValue(entity)});
  rig.scratch.Reset();
  CHECK(reply.ok && reply.values.size() == 1);
  CHECK(reply.values[0].as_string() == "Crate 01");

  // Entity ids survive the round trip including the generation half, which is
  // what stops a stale handle from being reused as a live one.
  ecs::Entity stale = rig.world.Create();
  rig.world.Destroy(stale);
  ecs::Entity live = rig.world.Create();
  CHECK(live.index == stale.index && live.generation != stale.generation);
  const rx::i64 stale_id =
      static_cast<rx::i64>(static_cast<rx::u64>(stale.index) |
                           (static_cast<rx::u64>(stale.generation) << 32));
  reply = rig.Call("World.IsValid", {rpc::RpcValue(stale_id)});
  CHECK(reply.ok && reply.values[0].as_bool() == false);
}

void TestRejection() {
  Rig rig;

  // Too few args for the signature.
  authoring::CommandBridge::Reply reply = rig.Call("World.Teleport", {rpc::RpcValue(rx::i64(1))});
  CHECK(!reply.ok);
  CHECK(reply.error.find("expects 4 arg(s) for (entity, vec3)") != std::string::npos);

  // Right arity, wrong type: a float where an entity id is wanted is a caller
  // bug, and truncating it would move some other entity.
  reply = rig.Call("World.Teleport", {rpc::RpcValue(1.5), rpc::RpcValue(0.0),
                                      rpc::RpcValue(0.0), rpc::RpcValue(0.0)});
  CHECK(!reply.ok);
  CHECK(reply.error.find("arg 0 expects entity, got float") != std::string::npos);

  // A string where a number is wanted, inside the vec3 expansion.
  reply = rig.Call("World.Teleport", {rpc::RpcValue(rx::i64(1)), rpc::RpcValue(0.0),
                                      rpc::RpcValue(std::string("up")), rpc::RpcValue(0.0)});
  CHECK(!reply.ok);
  CHECK(reply.error.find("arg 2 expects float") != std::string::npos);

  reply = rig.Call("World.DoesNotExist", {});
  CHECK(!reply.ok);
  CHECK(reply.error.find("unknown command") != std::string::npos);
}

void TestTrustGate() {
  Rig rig;
  ecs::Entity e = rig.world.Create();
  rig.world.Add(e, scene::Transform{{1, 2, 3}});
  const rx::i64 id = static_cast<rx::i64>(e.index);
  const rpc::RpcCall move{"World.Teleport",
                          {rpc::RpcValue(id), rpc::RpcValue(9.0), rpc::RpcValue(9.0),
                           rpc::RpcValue(9.0)}};

  // A remote game peer: any peer id the net path could attribute a packet to.
  authoring::CommandBridge::Reply reply = rig.bridge->Invoke(rpc::RpcContext{7, false}, move);
  CHECK(!reply.ok);
  CHECK(reply.error.find("local-endpoint only") != std::string::npos);

  // The host, on a client build. Also not the local authoring endpoint.
  reply = rig.bridge->Invoke(rpc::RpcContext{authoring::kLocalSender, true}, move);
  CHECK(!reply.ok);

  // Nothing moved: a refusal happens before any marshalling or dispatch.
  CHECK(rig.world.Get<scene::Transform>(e)->position[0] == 1.0f);

  // Dispatching straight through the registry, bypassing Invoke, also cannot
  // reach a handler: there is no reply slot, so the bound handler does nothing.
  CHECK(rig.bridge->registry().Dispatch(rpc::RpcContext{authoring::kLocalSender, false}, move));
  CHECK(rig.world.Get<scene::Transform>(e)->position[0] == 1.0f);
}

// The real socket path: frame in, frame out, over a live CommandEndpoint.
void TestEndpoint() {
  Rig rig;
  authoring::CommandEndpoint endpoint;
  const std::string path = "/tmp/rx_command_test_" + std::to_string(::getpid()) + ".sock";
  std::string error;
  if (!endpoint.Start(path, &error)) {
    std::printf("FAIL endpoint start: %s\n", error.c_str());
    ++g_failures;
    return;
  }
  CHECK(endpoint.running());

  // A second endpoint must not steal a live socket.
  authoring::CommandEndpoint duplicate;
  CHECK(!duplicate.Start(path, &error));

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size());
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

  ecs::Entity e = rig.world.Create();
  rig.world.Add(e, scene::Transform{{0, 0, 0}});
  const rpc::RpcCall call{"World.Teleport",
                          {rpc::RpcValue(static_cast<rx::i64>(e.index)), rpc::RpcValue(1.0),
                           rpc::RpcValue(2.0), rpc::RpcValue(3.0)}};
  const std::vector<u8> payload = rpc::EncodeCall(call);
  std::vector<u8> frame;
  for (int i = 0; i < 4; ++i)
    frame.push_back(static_cast<u8>(static_cast<u32>(payload.size()) >> (8 * i)));
  frame.insert(frame.end(), payload.begin(), payload.end());
  CHECK(::write(fd, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()));

  endpoint.Poll(*rig.bridge);  // accepts, reads, dispatches, replies

  u8 header[4];
  CHECK(::read(fd, header, 4) == 4);
  const u32 length = u32(header[0]) | u32(header[1]) << 8 | u32(header[2]) << 16 |
                     u32(header[3]) << 24;
  std::vector<u8> reply_bytes(length);
  CHECK(::read(fd, reply_bytes.data(), length) == static_cast<ssize_t>(length));
  std::optional<rpc::RpcCall> reply = rpc::DecodeCall(reply_bytes.data(), reply_bytes.size());
  CHECK(reply && reply->name == "ok");
  CHECK(rig.world.Get<scene::Transform>(e)->position[2] == 3.0f);

  // A client that sends and leaves before reading: the reply write finds a dead
  // peer, which must drop the connection rather than raise SIGPIPE and take the
  // engine with it.
  const int abandoning = ::socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(::connect(abandoning, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
  CHECK(::write(abandoning, frame.data(), frame.size()) ==
        static_cast<ssize_t>(frame.size()));
  ::close(abandoning);
  endpoint.Poll(*rig.bridge);
  endpoint.Poll(*rig.bridge);  // survived, and still serving

  ::close(fd);
  endpoint.Stop();
  CHECK(!endpoint.running());
  // Stop unlinks the socket, so nothing is left behind for the next run to trip
  // over (or for another user to connect to).
  const int gone = ::socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(::connect(gone, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0);
  ::close(gone);
}

}  // namespace

int main() {
  TestMarshalling();
  TestRejection();
  TestTrustGate();
  TestEndpoint();
  if (g_failures == 0) {
    std::printf("command_bridge_test: all checks passed\n");
    return 0;
  }
  std::printf("command_bridge_test: %d failure(s)\n", g_failures);
  return 1;
}
