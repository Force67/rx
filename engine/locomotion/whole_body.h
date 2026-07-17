#ifndef RX_LOCOMOTION_WHOLE_BODY_H_
#define RX_LOCOMOTION_WHOLE_BODY_H_

// Whole-body target generation (docs/LOCOMOTION.md §whole_body). Maps measured
// physical state plus the tick's contact/gait/footstep plans to the continuous
// numeric targets the joint-motor layer consumes: joint-local child-relative-
// to-parent rotations (identity = bind), per-group motor drive scales, and a
// pair of bounded root assists. This module never touches the physics engine;
// it is pure deterministic math, finite for every input (zero vectors, zero
// heights, non-finite reads all fall back).
//
// Conventions (shared with rx::scene / rx::character): right-handed, +Y up,
// facing yaw 0 looks down -Z, character-right = +X. Bind pose: legs straight
// down (-Y), arms straight down. Metres, m/s, radians, N and N*m.

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "locomotion/gait.h"
#include "locomotion/rig.h"
#include "locomotion/types.h"

namespace rx::locomotion {

// Result of the analytic 2-bone leg solve, expressed as joint-local deltas from
// the bind pose (identity = bind) in the pelvis (parent) frame.
struct LegIkResult {
  Quat hip{};            // hip swing-twist delta from bind
  f32 knee_flexion = 0;  // non-negative hinge angle about the knee axis (-X)
  Quat ankle{};          // ankle swing delta keeping the sole plane on target
  bool clamped = false;  // target was unreachable / degenerate and got clamped
};

// Analytic 2-bone leg inverse kinematics in the parent (pelvis) frame.
// `hip_to_sole` is the desired sole centre relative to the hip pivot, expressed
// in the pelvis frame (bind: straight leg = {0, -(upper+lower+foot_height), 0}).
// `sole_normal_parent` is the desired ground/sole normal in the same frame.
// The ankle joint sits `foot_height` above the sole along that normal, so the
// solved ankle point is `hip_to_sole + normalize(sole_normal_parent)*foot_height`.
// Produces the hip delta-from-bind, a non-negative knee flexion angle, and an
// ankle delta that rotates the foot's bind normal (+Y) onto `sole_normal_parent`
// (tilt clamped to a ~0.5 rad cone, shortest-arc so no shank twist). Unreachable
// targets clamp the chord to the straight-leg length and set `clamped`; a
// zero-length / non-finite target returns a bind-ish result with `clamped`.
// The knee axis is -X for BOTH legs (positive = flexion, foot swings +Z back).
RX_LOCOMOTION_EXPORT LegIkResult SolveLegIk(const Vec3& hip_to_sole, const Vec3& sole_normal_parent,
                                            f32 upper_length, f32 lower_length, f32 foot_height);

// Builds the tick's continuous whole-body targets from measured state + plans.
// Every field of `*out` is written on every call (no stale state carried).
RX_LOCOMOTION_EXPORT void BuildWholeBodyTargets(
    const CharacterMeasurements& m, const ContactEstimate& contacts, const GaitState& gait,
    const FootPlan plan[kFootCount], const LocomotionIntent& intent,
    const PhysicalModifiers& modifiers, const ControllerParameters& params, const BipedRig& rig,
    WholeBodyTargets* out);

}  // namespace rx::locomotion

#endif  // RX_LOCOMOTION_WHOLE_BODY_H_
