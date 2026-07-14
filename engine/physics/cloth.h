#ifndef RX_PHYSICS_CLOTH_H_
#define RX_PHYSICS_CLOTH_H_

#include "core/math.h"
#include "core/types.h"

namespace rx::physics {

// Opaque cloth handle; 0 is invalid.
using ClothId = u64;

enum class ClothBendModel : u8 {
  kNone,
  kDistance,
  kDihedral,
};

enum class ClothLraMode : u8 {
  kNone,
  kEuclidean,
  kGeodesic,
};

struct ClothSkinWeight {
  u32 joint = 0;
  f32 weight = 0;
};

// A sparse native Jolt skin constraint. Inverse-bind matrices transform the
// descriptor's model-space positions into joint-local space. Joint transforms
// supplied at runtime are world-space matrices.
struct ClothSkinConstraint {
  u32 vertex = 0;
  ClothSkinWeight weights[4] = {};
  f32 max_distance = 3.402823466e+38F;
  f32 backstop_distance = 3.402823466e+38F;
  f32 backstop_radius = 0;
};

// Immutable creation data for an arbitrary welded triangle simulation mesh.
// Render meshes can be denser and follow this cage through an asset-authored
// barycentric binding. Positions are in metres before `transform` is applied.
struct ClothDesc {
  const Vec3* positions = nullptr;
  u32 vertex_count = 0;
  const u32* indices = nullptr;
  u32 index_count = 0;

  // Optional material UVs (u, v pairs). They orient warp/weft stiffness.
  const f32* uvs = nullptr;
  // Optional per-vertex inverse masses. Without these, masses are derived from
  // triangle area and areal_density. `pins` always override inverse mass to 0.
  const f32* inverse_masses = nullptr;
  const u32* pins = nullptr;
  u32 pin_count = 0;

  // Optional skeletal attachment. Each joint has one inverse-bind matrix and
  // skin constraints are sparse, so unskinned cloth pays no runtime cost.
  const Mat4* inverse_bind_matrices = nullptr;
  u32 joint_count = 0;
  const ClothSkinConstraint* skin_constraints = nullptr;
  u32 skin_constraint_count = 0;

  // Fabric response. Compliance is inverse stiffness; 0 is inextensible.
  f32 areal_density = 0.3f;  // kg / m^2
  f32 warp_compliance = 0;
  f32 weft_compliance = 0;
  f32 shear_compliance = 1.0e-5f;
  f32 bend_compliance = 1.0e-3f;
  ClothBendModel bend_model = ClothBendModel::kDihedral;
  ClothLraMode lra_mode = ClothLraMode::kGeodesic;
  f32 max_stretch = 1.02f;

  // Jolt's native soft-body controls.
  u32 iterations = 5;
  f32 damping = 0.05f;
  f32 gravity_factor = 1;
  f32 friction = 0.4f;
  f32 restitution = 0;
  f32 pressure = 0;  // Jolt n*R*T; one outward-wound closed shell only
  f32 collision_radius = 0.005f;
  f32 max_linear_velocity = 100;
  bool update_position = true;
  bool allow_sleeping = true;
  bool faces_double_sided = true;

  // Engine extension over Jolt: continuous vertex/triangle and edge/edge
  // self-collision with one-ring topology exclusion. A swept BVH bounds the
  // candidate set for irregular and fast-moving meshes. Set distance to 0 to
  // disable it.
  f32 self_collision_distance = 0.01f;
  u32 self_collision_iterations = 2;
  f32 self_collision_relaxation = 0.9f;

  // Aerodynamic drag coefficient. SetClothWind supplies air velocity in m/s.
  f32 aerodynamic_drag = 1;
};

}  // namespace rx::physics

#endif  // RX_PHYSICS_CLOTH_H_
