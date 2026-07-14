#ifndef RX_PHYSICS_CLOTH_COLLISION_H_
#define RX_PHYSICS_CLOTH_COLLISION_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "core/types.h"

namespace rx::physics::detail {

struct ClothTopology {
  base::Vector<u32> indices;
  base::Vector<u32> edges;
  base::Vector<f32> triangle_areas;
  base::Vector<u32> neighbor_offsets;
  base::Vector<u32> neighbors;
  f32 average_edge_length = 0;
  f32 signed_volume = 0;
  u32 component_count = 0;
  bool closed = false;
};

// Validates a welded, consistently indexed triangle mesh and builds the static
// topology used by the runtime collision pass. Open and closed meshes and
// disconnected panels are accepted; non-manifold and degenerate input is not.
bool BuildClothTopology(const Vec3* positions, u32 vertex_count,
                        const u32* indices, u32 index_count,
                        ClothTopology* out);

struct ClothSelfCollisionConfig {
  f32 distance = 0;
  f32 relaxation = 1;
  f32 max_velocity = 100;
  u32 iterations = 1;
};

struct ClothSelfCollisionScratch {
  struct Bounds {
    Vec3 low;
    Vec3 high;
  };
  struct BvhNode {
    Bounds bounds;
    u32 left = 0;
    u32 right = 0;
    u32 begin = 0;
    u32 count = 0;
  };

  base::Vector<Vec3> predicted;
  base::Vector<Bounds> primitive_bounds;
  base::Vector<u32> primitives;
  base::Vector<BvhNode> bvh;
};

// Applies velocity corrections for contacts in the predicted state. Jolt then
// integrates those velocities and solves its native structural constraints.
// Returns the total number of projected vertex/triangle and edge/edge contacts.
u32 SolveClothSelfCollision(const ClothTopology& topology,
                            const ClothSelfCollisionConfig& config,
                            const base::Vector<Vec3>& positions,
                            base::Vector<Vec3>* velocities,
                            const base::Vector<f32>& inverse_masses, f32 dt,
                            ClothSelfCollisionScratch* scratch);

}  // namespace rx::physics::detail

#endif  // RX_PHYSICS_CLOTH_COLLISION_H_
