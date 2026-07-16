#include "render/gi/rt_instance_cull.h"

#include <algorithm>
#include <cmath>

namespace rx::render {
namespace {

// Largest per-axis scale of the upper-left 3x3, so a non-uniform transform
// never shrinks the bounding sphere below the geometry it must enclose.
f32 MaxScale(const Mat4& t) {
  const f32* m = t.m;
  const f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  const f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
  const f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
  return std::max({sx, sy, sz});
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

const base::Vector<u8>& RtInstanceCuller::UpdateGroup(u32 group_id, u32 generation,
                                                      std::span<const Mat4> transforms,
                                                      const Vec3& mesh_center, f32 mesh_radius) {
  if (group_id >= groups_.size()) groups_.resize(group_id + 1);
  GroupState& gs = groups_[group_id];
  const u32 n = static_cast<u32>(transforms.size());

  // A fresh or reused slot, or a resized group, starts accept-all so a group
  // that just streamed in is fully present before its first sweep refines it.
  bool fresh = false;
  if (!gs.valid || gs.generation != generation || gs.visible.size() != n) {
    gs.generation = generation;
    gs.cursor = 0;
    gs.valid = true;
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
