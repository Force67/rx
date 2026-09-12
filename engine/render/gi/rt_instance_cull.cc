#include "render/gi/rt_instance_cull.h"

#include <algorithm>
#include <cmath>

namespace rx::render {
namespace {

// Bound the largest eigenvalue of M^T M by its maximum absolute row sum.
// Column lengths alone underestimate the stretch when a transform has shear.
f32 MaxScale(const Mat4& t) {
  const f32* m = t.m;
  f32 bound = 0.0f;
  for (u32 i = 0; i < 3; ++i) {
    f32 row = 0.0f;
    for (u32 j = 0; j < 3; ++j)
      row += std::abs(m[4 * i] * m[4 * j] + m[4 * i + 1] * m[4 * j + 1] +
                      m[4 * i + 2] * m[4 * j + 2]);
    bound = std::max(bound, row);
  }
  return std::sqrt(bound);
}

}  // namespace

bool RtInstanceCuller::DrawVisible(const Mat4& transform, const Vec3& mesh_center,
                                   f32 mesh_radius) const {
  if (!enabled_) return true;
  return !Cull(TransformPoint(transform, mesh_center), mesh_radius * MaxScale(transform));
}

bool RtInstanceCuller::Cull(const Vec3& c, f32 radius) const {
  if (radius <= 0.0f) return false;  // unknown bounds (radius 0): never cull
  const f32 dx = c.x - eye_.x, dy = c.y - eye_.y, dz = c.z - eye_.z;
  const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (dist <= start_distance_) return false;  // near field is always kept
  // Angular radius ~= radius / dist; drop when it is below the threshold.
  return radius < angle_threshold_ * dist;
}

void RtInstanceCuller::BeginFrame(const Vec3& camera_eye) {
  teleported_ = false;
  if (have_eye_) {
    const f32 dx = camera_eye.x - eye_.x, dy = camera_eye.y - eye_.y,
              dz = camera_eye.z - eye_.z;
    if (dx * dx + dy * dy + dz * dz > kTeleportDistance * kTeleportDistance) {
      teleported_ = true;
      // Accept-all everywhere; the incremental sweeps re-cull over ~1 s.
      for (GroupState& gs : groups_) {
        std::fill(gs.visible.begin(), gs.visible.end(), u8{1});
        gs.cursor = 0;
      }
    }
  }
  eye_ = camera_eye;
  have_eye_ = true;
}

const base::Vector<u8>& RtInstanceCuller::UpdateGroup(u32 group_id, u32 generation, u32 revision,
                                                      std::span<const Mat4> transforms,
                                                      const Vec3& mesh_center, f32 mesh_radius) {
  if (group_id >= groups_.size()) groups_.resize(group_id + 1);
  GroupState& gs = groups_[group_id];
  const u32 n = static_cast<u32>(transforms.size());

  // A fresh or reused slot, a resized group, or an in-place transform update
  // (revision bump) starts accept-all so a group that just streamed in -- or an
  // instance that just moved next to the camera -- is fully present before the
  // next sweep refines it. Without the revision check a same-count Replace keeps
  // the stale bitmask until the moved instance's sweep index is revisited.
  bool fresh = false;
  if (!gs.valid || gs.generation != generation || gs.revision != revision ||
      gs.visible.size() != n || gs.mesh_radius != mesh_radius ||
      gs.mesh_center.x != mesh_center.x || gs.mesh_center.y != mesh_center.y ||
      gs.mesh_center.z != mesh_center.z) {
    gs.generation = generation;
    gs.revision = revision;
    gs.cursor = 0;
    gs.valid = true;
    gs.mesh_center = mesh_center;
    gs.mesh_radius = mesh_radius;
    gs.visible.assign(n, u8{1});
    fresh = true;
  }

  // Disabled, just-teleported or just-initialised: keep everything, run no
  // sweep this frame (the sweep begins next frame from an accept-all base).
  if (!enabled_ || teleported_ || fresh || n == 0) return gs.visible;

  // Re-test a bounded slice, wrapping around; a full sweep lands over
  // ~kSweepFrames frames regardless of group size.
  u32 slice = std::max(kMinSlice, (n + kSweepFrames - 1) / kSweepFrames);
  slice = std::min(slice, n);
  for (u32 k = 0; k < slice; ++k) {
    const u32 i = (gs.cursor + k) % n;
    const Vec3 c = TransformPoint(transforms[i], mesh_center);
    const f32 r = mesh_radius * MaxScale(transforms[i]);
    gs.visible[i] = Cull(c, r) ? u8{0} : u8{1};
  }
  gs.cursor = (gs.cursor + slice) % n;
  return gs.visible;
}

}  // namespace rx::render
