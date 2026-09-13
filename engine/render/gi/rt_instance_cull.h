#ifndef RX_RENDER_RT_INSTANCE_CULL_H_
#define RX_RENDER_RT_INSTANCE_CULL_H_

#include <span>

#include <base/containers/vector.h>

#include "core/math.h"
#include "core/types.h"

namespace rx::render {

// Solid-angle + distance TLAS instance culling (AC Shadows "Ray tracing the
// world" §2): beyond a start distance, an instance whose projected angular
// radius (bounding-sphere radius / distance) is below a threshold is dropped
// from the realtime TLAS, so small distant clutter stops paying BLAS/traversal
// cost. Per-draw instances are few and tested inline every frame (DrawVisible);
// static groups can hold thousands, so each is swept incrementally (a slice per
// frame, full sweep ~1 s, kSweepFrames) while a persistent per-instance
// visibility bitmask drives inclusion, bounding per-frame cost regardless of
// group size.
//
// Culling only removes distant small geometry, so "keep everything" is always
// safe: on a large camera jump every group falls back to accept-all and the
// sweep re-converges within a second. Realtime-only: path tracer / reference
// modes keep the full instance set.
class RtInstanceCuller {
 public:
  // Frames a full per-group sweep is spread across (~1 s at 60 fps).
  static constexpr u32 kSweepFrames = 60;
  // Floor on the per-group slice so small groups finish in a frame or two and
  // the sweep never crawls when kSweepFrames would round down to a handful.
  static constexpr u32 kMinSlice = 128;
  // A single-frame camera translation beyond this (metres) is treated as a
  // teleport and forces every group back to accept-all.
  static constexpr f32 kTeleportDistance = 20.0f;

  void Configure(bool enabled, f32 start_distance, f32 angle_threshold) {
    if (enabled_ != enabled || start_distance_ != start_distance ||
        angle_threshold_ != angle_threshold) {
      for (GroupState& gs : groups_) gs.valid = false;
    }
    enabled_ = enabled;
    start_distance_ = start_distance;
    angle_threshold_ = angle_threshold;
  }
  bool enabled() const { return enabled_; }

  // Call once per frame, before walking the instance set. Detects teleports.
  void BeginFrame(const Vec3& camera_eye);

  // Inline test for a frustum-culled per-draw instance: transform is its world
  // matrix, (mesh_center, mesh_radius) the mesh model-space sphere. Returns true
  // to keep the instance in the TLAS.
  bool DrawVisible(const Mat4& transform, const Vec3& mesh_center, f32 mesh_radius) const;

  // Advances the time-sliced sweep for one static instance group and returns
  // its persistent visibility bitmask (1 = keep instance i). group_id indexes
  // the caller's dense group array; generation invalidates stale state when a
  // slot is reused for a different group. revision invalidates it when the same
  // group's transforms change in place (InstanceStore::Replace keeps generation
  // stable, so without this a moved-but-same-count instance stays culled until
  // its sweep index comes back around). transforms are the per-instance world
  // transforms; (mesh_center, mesh_radius) is the mesh's model-space sphere.
  const base::Vector<u8>& UpdateGroup(u32 group_id, u32 generation, u32 revision,
                                      std::span<const Mat4> transforms,
                                      const Vec3& mesh_center, f32 mesh_radius);

 private:
  struct GroupState {
    u32 generation = 0;
    u32 revision = 0;
    u32 cursor = 0;  // next instance index the sweep will (re)test
    bool valid = false;
    Vec3 mesh_center{};
    f32 mesh_radius = 0.0f;
    base::Vector<u8> visible;  // one byte per instance, 1 = keep
  };

  // True when the sphere should be dropped from the TLAS.
  bool Cull(const Vec3& world_center, f32 world_radius) const;

  bool enabled_ = true;
  f32 start_distance_ = 40.0f;
  f32 angle_threshold_ = 0.004f;
  Vec3 eye_{};
  bool have_eye_ = false;
  bool teleported_ = false;  // set for the frame a teleport is detected
  base::Vector<GroupState> groups_;
};

}  // namespace rx::render

#endif  // RX_RENDER_RT_INSTANCE_CULL_H_
