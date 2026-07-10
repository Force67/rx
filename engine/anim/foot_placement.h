#ifndef RX_ANIM_FOOT_PLACEMENT_H_
#define RX_ANIM_FOOT_PLACEMENT_H_

#include <functional>
#include <memory>

#include "anim/anim_graph.h"
#include "anim/pose.h"
#include "core/math.h"

namespace rx::anim {

// Ground probe in the actor's MODEL space: cast down from `origin`; on a hit,
// fill the model-space contact point and surface normal, else return false. The
// app wraps its physics world and converts to/from the actor model space -
// engine/anim links no physics, exactly as kinema itself keeps raycasts out of
// its solvers.
using GroundProbe = std::function<bool(const Vec3& origin, Vec3* hit, Vec3* normal)>;

// Grounds a kinema-driven pose: probes under each foot, drops the pelvis to the
// lower foot and runs analytic two-bone leg IK onto the contacts
// (kinema::SolveFootPlacement). Bound once to an AnimGraph (resolving the pelvis
// and the L/R hip/knee/ankle of its biped rig), then Apply()'d each frame after
// RigPlayer::Update. Owns its model-space scratch, so the per-frame path never
// allocates.
class FootPlacement {
 public:
  FootPlacement();
  ~FootPlacement();
  FootPlacement(FootPlacement&&) noexcept;
  FootPlacement& operator=(FootPlacement&&) noexcept;
  FootPlacement(const FootPlacement&) = delete;
  FootPlacement& operator=(const FootPlacement&) = delete;

  // Resolve the rig joints from the graph's skeleton (the "NPC ... [ ]" biped
  // convention). `ankle_height` is the distance from the ankle joint to the
  // sole. Returns false if the skeleton lacks the expected joints.
  bool Bind(const AnimGraph& graph, f32 ankle_height = 0.08f);
  bool bound() const;

  // Solve in place over `pose` (model-space up = +Y). Returns the signed pelvis
  // offset applied along up (<= 0: the body sank to the lower foot).
  f32 Apply(SkeletonPose* pose, const GroundProbe& probe);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rx::anim

#endif  // RX_ANIM_FOOT_PLACEMENT_H_
