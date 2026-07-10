#ifndef RX_ANIM_ANIM_INTERNAL_H_
#define RX_ANIM_ANIM_INTERNAL_H_

// Internal, kinema-visible glue shared across the engine/anim translation units
// (anim_graph.cc, rig_player.cc, foot_placement.cc). It is deliberately NOT part
// of the installed public headers: kinema is a private implementation detail of
// engine/anim, exactly as it is of engine/physics (see kinema_jolt.cc), so no
// public rx header includes <kinema/kinema.h>. cmake/install.cmake excludes
// *_internal.h from the package for this reason.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <kinema/kinema.h>

#include "anim/pose.h"
#include "asset/skeleton.h"
#include "core/math.h"

namespace rx::anim::detail {

// rx and kinema grew from the same TRS lineage; the pose SoA is bit-compatible,
// so a kinema PoseView can alias an rx SkeletonPose's arrays with no copy. These
// asserts fail the build the moment either layout drifts.
static_assert(sizeof(Vec3) == sizeof(kinema::Vec3), "Vec3 layout drift");
static_assert(sizeof(Quat) == sizeof(kinema::Quat), "Quat layout drift");
static_assert(offsetof(Vec3, x) == offsetof(kinema::Vec3, x), "Vec3.x drift");
static_assert(offsetof(Vec3, z) == offsetof(kinema::Vec3, z), "Vec3.z drift");
static_assert(offsetof(Quat, w) == offsetof(kinema::Quat, w), "Quat.w drift");

inline kinema::PoseView AsKinema(SkeletonPose& p) {
  return kinema::PoseView{reinterpret_cast<kinema::Vec3*>(p.translation.data()),
                          reinterpret_cast<kinema::Quat*>(p.rotation.data()), p.scale.data(),
                          p.size()};
}

inline Vec3 ToRx(const kinema::Vec3& v) { return Vec3{v.x, v.y, v.z}; }

// asset::Skeleton -> kinema::Skeleton: parents, FNV name hashes and bind TRS.
// Built once per archetype (parents already precede children).
kinema::Skeleton BuildKinemaSkeleton(const asset::Skeleton& skeleton);

// The immutable archetype payload: the kinema skeleton binding, the owned clips
// (stable addresses - the compiled machine and blend space hold raw pointers
// into them), the compiled state machine, the named parameter layout and the
// role indices the RigPlayer/FootPlacement need. Held behind a shared_ptr by
// AnimGraph and shared by every character of the archetype.
struct GraphState {
  kinema::Skeleton skeleton;
  std::vector<std::unique_ptr<kinema::OwnedClip>> clips;  // stable Clip addresses
  std::unique_ptr<kinema::BlendSpace> locomotion_space;
  kinema::StateMachine machine;
  std::vector<std::string> param_names;  // index == parameter id

  // Locomotion archetype roles (indices into `clips`, -1 if absent).
  int idle_clip = -1, walk_clip = -1, run_clip = -1;
  u16 idle_state = 0, loco_state = 1;
  int speed_param = -1, phase_param = -1;
  f32 walk_speed = 1.6f;  // authored planar speed of the walk clip (m/s)
  f32 run_speed = 4.5f;   // authored planar speed of the run clip (m/s)
  u64 footstep_curve = 0;  // hashed name of the footstep-intensity curve

  const kinema::Clip* clip(int role) const {
    return role >= 0 ? clips[static_cast<size_t>(role)]->get() : nullptr;
  }
};

}  // namespace rx::anim::detail

#endif  // RX_ANIM_ANIM_INTERNAL_H_
