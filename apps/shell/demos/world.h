#ifndef RX_APPS_SHELL_DEMOS_WORLD_H_
#define RX_APPS_SHELL_DEMOS_WORLD_H_


#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "base/strings/xstring.h"
#include "rxe/asset/vfs.h"
#include "rxe/ecs/world.h"
#include "rxe/render/core/renderer.h"
#include "rxe/world/world_claim.h"
#include "rxe/world/world_map.h"
#include "rxe/world/world_stream.h"

namespace rx::shell {

// `--world <archive.rxp>`: the viewer driving a baked world.
//
// This is the consumer rxe/world was written for, and until it existed the
// module had never been driven by anything but its own tests. It is deliberately
// thin - mount, load the index, hand the camera to the streamer once a frame,
// draw what came back - because that is the whole of what a host has to do, and
// anything thicker here would be the module failing to carry its own weight.
//
// The two halves of the design show up as two different jobs:
//
//   Gameplay cells materialize real ECS entities carrying Transform and
//   Renderable, so app::Host::GatherEntityDraws finds them with everything
//   else and this class does nothing at all for them.
//
//   Representation cells stay out of the ECS. Their rows are transforms in a
//   page, and EmitToView walks the resident pages and emits draws directly.
//   That is the point of the split: most of a world costs no entity.
class WorldStreamDemo {
 public:
  // False, with the reason logged, when the archive is missing or its index
  // will not load. `world_name` is the name the archive was baked under, which
  // is also the directory the index sits in.
  bool Init(asset::Vfs& vfs, render::Renderer* renderer, ecs::World& ecs, bool headless,
            const base::String& archive_path, const base::String& world_name);

  bool active() const { return streamer_ != nullptr; }

  // One tick: the camera becomes the streaming source. Velocity is derived here
  // rather than taken from the camera because the planner wants the observer's
  // recent motion, not its input.
  void Update(f32 frame_delta, const Vec3& camera_position);

  void EmitToView(render::FrameView& view);

  // Gives every entity the world materialized back, and drops the streamer.
  // The host owns the ecs::World and tears it down when it stops; a streamer
  // outliving it would destroy entities out of a freed world, so this has to
  // run from OnShutdown rather than from ~Viewer, which is later.
  void Shutdown();

  // Overlay line: what is resident, what is in flight, what failed.
  base::String StatusLine() const;

 private:
  void RegisterMeshes(render::Renderer* renderer, bool headless);

  world::WorldMap map_;
  base::UniquePointer<world::CellLoader> loader_;
  base::UniquePointer<world::WorldStreamer> streamer_;
  world::ClaimSet claims_;
  base::String world_name_;

  Vec3 previous_position_;
  bool have_previous_ = false;

  base::Vector<u64> resident_scratch_;
  base::Vector<u64> prototype_ids_scratch_;
  u64 reported_errors_ = 0;
};

}  // namespace rx::shell

#endif  // RX_APPS_SHELL_DEMOS_WORLD_H_
