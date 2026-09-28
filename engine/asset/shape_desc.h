#ifndef RX_ASSET_SHAPE_DESC_H_
#define RX_ASSET_SHAPE_DESC_H_

// Engine-neutral collision shape description: a tree of primitives,
// convex hulls and placed children that PhysicsWorld lowers into Jolt
// shapes. Producers include the Havok .hkx decoder (e.g. authored game
// collision/ragdolls) and, eventually, NIF bhk blocks; nothing in here is
// format-specific. Plain data with no physics behind it, so it lives with the
// other asset formats: an item catalog can carry one without linking Jolt.


#include "base/containers/vector.h"
#include "foundation/build_config/types.h"
#include "foundation/math/math.h"

namespace rx::asset {

struct ShapeDesc {
  enum class Kind { kSphere, kCapsule, kBox, kConvexHull, kCompound, kPlaced, kInvalid };
  Kind kind = Kind::kInvalid;
  f32 radius = 0;       // sphere; capsule
  Vec3 a{}, b{};        // capsule segment ends (local space)
  Vec3 half_extents{};  // box
  base::Vector<Vec3> vertices;      // convex hull
  base::Vector<ShapeDesc> children;  // compound members / the placed child
  // kPlaced child placement: four float4 COLUMNS (basis c0, c1, c2, origin).
  f32 transform[16] = {};
};

}  // namespace rx::asset

#endif  // RX_ASSET_SHAPE_DESC_H_
