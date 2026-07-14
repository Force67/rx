#include <base/containers/vector.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/feature_registry.h"
#include "core/math.h"
#include "physics/physics_world.h"

using namespace rx;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "cloth_sim_test FAIL: %s\n", what);
  return 1;
}

bool IsFinite(const Vec3& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

struct Mesh {
  base::Vector<Vec3> positions;
  base::Vector<f32> uvs;
  base::Vector<u32> indices;
  base::Vector<u32> pins;
};

Mesh MakeCurtain(u32 width, u32 height, f32 spacing) {
  Mesh mesh;
  mesh.positions.reserve(width * height);
  mesh.uvs.reserve(width * height * 2);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      mesh.positions.push_back(
          {(static_cast<f32>(x) - static_cast<f32>(width - 1) * 0.5f) * spacing,
           -static_cast<f32>(y) * spacing, 0});
      mesh.uvs.push_back(static_cast<f32>(x) / static_cast<f32>(width - 1));
      mesh.uvs.push_back(static_cast<f32>(y) / static_cast<f32>(height - 1));
    }
  }
  for (u32 y = 0; y + 1 < height; ++y) {
    for (u32 x = 0; x + 1 < width; ++x) {
      const u32 a = y * width + x;
      const u32 b = a + 1;
      const u32 c = a + width;
      const u32 d = c + 1;
      mesh.indices.push_back(a);
      mesh.indices.push_back(c);
      mesh.indices.push_back(b);
      mesh.indices.push_back(b);
      mesh.indices.push_back(c);
      mesh.indices.push_back(d);
    }
  }
  for (u32 x = 0; x < width; ++x) mesh.pins.push_back(x);
  return mesh;
}

Mesh MakeSkirt(u32 segments, u32 rings) {
  Mesh mesh;
  mesh.positions.reserve(segments * rings);
  for (u32 ring = 0; ring < rings; ++ring) {
    const f32 t = static_cast<f32>(ring) / static_cast<f32>(rings - 1);
    const f32 radius = 0.26f + 0.12f * t;
    for (u32 segment = 0; segment < segments; ++segment) {
      const f32 angle = 6.28318530718f * static_cast<f32>(segment) /
                        static_cast<f32>(segments);
      mesh.positions.push_back(
          {radius * std::cos(angle), -0.7f * t, radius * std::sin(angle)});
    }
  }
  for (u32 ring = 0; ring + 1 < rings; ++ring) {
    for (u32 segment = 0; segment < segments; ++segment) {
      const u32 next = (segment + 1) % segments;
      const u32 a = ring * segments + segment;
      const u32 b = ring * segments + next;
      const u32 c = (ring + 1) * segments + segment;
      const u32 d = (ring + 1) * segments + next;
      mesh.indices.push_back(a);
      mesh.indices.push_back(c);
      mesh.indices.push_back(b);
      mesh.indices.push_back(b);
      mesh.indices.push_back(c);
      mesh.indices.push_back(d);
    }
  }
  for (u32 segment = 0; segment < segments; ++segment)
    mesh.pins.push_back(segment);
  return mesh;
}

physics::ClothDesc DescFor(const Mesh& mesh) {
  physics::ClothDesc desc;
  desc.positions = mesh.positions.data();
  desc.vertex_count = static_cast<u32>(mesh.positions.size());
  desc.indices = mesh.indices.data();
  desc.index_count = static_cast<u32>(mesh.indices.size());
  desc.uvs = mesh.uvs.empty() ? nullptr : mesh.uvs.data();
  desc.pins = mesh.pins.data();
  desc.pin_count = static_cast<u32>(mesh.pins.size());
  desc.areal_density = 0.25f;
  desc.shear_compliance = 2.0e-6f;
  desc.bend_compliance = 2.0e-4f;
  desc.iterations = 8;
  desc.damping = 0.08f;
  desc.collision_radius = 0.008f;
  desc.self_collision_distance = 0.016f;
  return desc;
}

bool ReadCloth(physics::PhysicsWorld& world, physics::ClothId id,
               base::Vector<Vec3>* positions) {
  const u32 count = world.ClothVertexCount(id);
  if (count == 0) return false;
  positions->resize(count);
  if (!world.GetClothPositions(id, positions->data(), count)) return false;
  for (u32 i = 0; i < positions->size(); ++i) {
    if (!IsFinite((*positions)[i])) {
      std::fprintf(stderr, "non-finite cloth vertex %u: %g %g %g\n", i,
                   (*positions)[i].x, (*positions)[i].y, (*positions)[i].z);
      return false;
    }
  }
  return true;
}

int TestCurtain(physics::PhysicsWorld& world) {
  constexpr u32 kWidth = 13;
  constexpr u32 kHeight = 15;
  constexpr f32 kSpacing = 0.06f;
  const Mesh mesh = MakeCurtain(kWidth, kHeight, kSpacing);
  physics::ClothDesc desc = DescFor(mesh);
  const Mat4 rest = MakeTranslation({0, 1.8f, 0});
  const physics::ClothId cloth = world.CreateCloth(desc, rest);
  if (cloth == 0) return Fail("CreateCloth curtain");
  if (world.ClothVertexCount(cloth) != mesh.positions.size()) {
    return Fail("ClothVertexCount curtain");
  }

  for (u32 step = 0; step < 180; ++step) world.Update(kDt);
  base::Vector<Vec3> positions;
  if (!ReadCloth(world, cloth, &positions))
    return Fail("finite curtain readback");
  for (u32 x = 0; x < kWidth; ++x) {
    if (Length(positions[x] - TransformPoint(rest, mesh.positions[x])) >
        3.0e-3f) {
      return Fail("curtain top row did not remain pinned");
    }
  }
  f32 bottom_y = 0;
  for (u32 x = 0; x < kWidth; ++x)
    bottom_y += positions[(kHeight - 1) * kWidth + x].y;
  bottom_y /= static_cast<f32>(kWidth);
  if (bottom_y > 1.8f - kSpacing * static_cast<f32>(kHeight - 1) + 0.08f) {
    return Fail("curtain did not hang under gravity");
  }
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    const u32 a = mesh.indices[i + 0], b = mesh.indices[i + 1],
              c = mesh.indices[i + 2];
    for (const auto edge :
         {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
      const f32 rest_length =
          Length(mesh.positions[edge.first] - mesh.positions[edge.second]);
      if (Length(positions[edge.first] - positions[edge.second]) >
          rest_length * 1.15f) {
        return Fail("curtain structural edge stretched too far");
      }
    }
  }

  world.SetClothWind(cloth, {0, 0, 8});
  for (u32 step = 0; step < 120; ++step) world.Update(kDt);
  if (!ReadCloth(world, cloth, &positions))
    return Fail("wind curtain readback");
  f32 average_z = 0;
  for (u32 y = 1; y < kHeight; ++y) {
    for (u32 x = 0; x < kWidth; ++x) average_z += positions[y * kWidth + x].z;
  }
  average_z /= static_cast<f32>((kHeight - 1) * kWidth);
  if (average_z < 0.03f) return Fail("aerodynamic wind did not move curtain");

  world.SetClothWind(cloth, {});
  Mat4 moved = MakeTranslation({0.3f, 1.8f, 0});
  if (!world.SetClothTransform(cloth, moved, 0.5f)) {
    return Fail("retarget pinned curtain transform");
  }
  for (u32 step = 0; step < 90; ++step) world.Update(kDt);
  if (!ReadCloth(world, cloth, &positions))
    return Fail("moved curtain readback");
  for (u32 x = 0; x < kWidth; ++x) {
    if (Length(positions[x] - TransformPoint(moved, mesh.positions[x])) >
        4.0e-3f) {
      return Fail("moving curtain pins did not track");
    }
  }

  world.RemoveCloth(cloth);
  if (world.ClothVertexCount(cloth) != 0)
    return Fail("removed curtain handle stayed alive");
  return 0;
}

int TestSkirt(physics::PhysicsWorld& world) {
  constexpr u32 kSegments = 18;
  constexpr u32 kRings = 10;
  const Mesh mesh = MakeSkirt(kSegments, kRings);
  physics::ClothDesc desc = DescFor(mesh);
  desc.bend_compliance = 8.0e-5f;
  const Mat4 transform = MakeTranslation({1.4f, 1.55f, 0});
  const physics::BodyId collider =
      world.AddKinematicCapsule({1.4f, 1.15f, 0}, 0.30f, 0.38f);
  if (collider == 0) return Fail("skirt character collider");
  const physics::ClothId skirt = world.CreateCloth(desc, transform);
  if (skirt == 0) return Fail("CreateCloth cylindrical skirt");
  for (u32 step = 0; step < 240; ++step) world.Update(kDt);

  base::Vector<Vec3> positions;
  if (!ReadCloth(world, skirt, &positions))
    return Fail("finite skirt readback");
  for (u32 i = 0; i < kSegments; ++i) {
    if (Length(positions[i] - TransformPoint(transform, mesh.positions[i])) >
        4.0e-3f) {
      return Fail("skirt waist did not remain attached");
    }
  }
  for (u32 ring = 1; ring < kRings; ++ring) {
    for (u32 segment = 0; segment < kSegments; ++segment) {
      const Vec3 p = positions[ring * kSegments + segment] - Vec3{1.4f, 0, 0};
      if (std::sqrt(p.x * p.x + p.z * p.z) < 0.27f) {
        return Fail("skirt penetrated character capsule");
      }
    }
  }
  world.RemoveCloth(skirt);
  world.RemoveBody(collider);
  return 0;
}

int TestSkinning(physics::PhysicsWorld& world) {
  Mesh mesh = MakeCurtain(3, 3, 0.12f);
  mesh.pins.clear();
  physics::ClothDesc desc = DescFor(mesh);
  const Mat4 inverse_bind = Mat4::Identity();
  physics::ClothSkinConstraint skin[9];
  for (u32 i = 0; i < 9; ++i) {
    skin[i].vertex = i;
    skin[i].weights[0] = {0, 1};
    skin[i].max_distance = i < 3 ? 0 : 0.04f;
    if (i >= 3) {
      skin[i].backstop_distance = -0.01f;
      skin[i].backstop_radius = 0.02f;
    }
  }
  desc.inverse_bind_matrices = &inverse_bind;
  desc.joint_count = 1;
  desc.skin_constraints = skin;
  desc.skin_constraint_count = 9;
  desc.gravity_factor = 0;
  const Quat rotation = QuatFromAxisAngle({0, 1, 0}, 0.35f);
  const Mat4 spawn = MakeTransform({-1.2f, 1.4f, 0}, rotation, 1.2f);
  physics::ClothDesc partial_backstop = desc;
  partial_backstop.skin_constraints = &skin[4];
  partial_backstop.skin_constraint_count = 1;
  if (world.CreateCloth(partial_backstop, spawn) != 0) {
    return Fail("backstop on a partially skinned face was accepted");
  }
  const u32 conflicting_pin = 0;
  physics::ClothDesc conflicting = desc;
  conflicting.pins = &conflicting_pin;
  conflicting.pin_count = 1;
  if (world.CreateCloth(conflicting, spawn) != 0) {
    return Fail("conflicting pin and skin attachment was accepted");
  }
  const physics::ClothId cloth = world.CreateCloth(desc, spawn);
  if (cloth == 0) return Fail("CreateCloth skinned panel");
  if (world.SetClothTransform(cloth, spawn, kDt)) {
    return Fail("unpinned cloth transform was accepted");
  }
  world.Update(kDt);
  base::Vector<Vec3> positions;
  if (!ReadCloth(world, cloth, &positions)) {
    return Fail("uninitialized skin constraints affected simulation");
  }
  Mat4 joint = spawn;
  if (!world.SetClothJointTransforms(cloth, &joint, 1)) {
    return Fail("initialize skinned cloth");
  }
  joint = MakeTransform({-0.9f, 1.5f, 0.1f}, rotation, 1.2f);
  for (u32 step = 0; step < 120; ++step) {
    if (!world.SetClothJointTransforms(cloth, &joint, 1))
      return Fail("update cloth skinning");
    world.Update(kDt);
  }
  if (!ReadCloth(world, cloth, &positions))
    return Fail("skinned cloth readback");
  for (u32 i = 0; i < 3; ++i) {
    if (Length(positions[i] - TransformPoint(joint, mesh.positions[i])) >
        4.0e-3f) {
      return Fail("native skin constraint did not track joint");
    }
  }
  world.RemoveCloth(cloth);
  return 0;
}

int TestPressureAndValidation(physics::PhysicsWorld& world) {
  Mesh open = MakeCurtain(3, 3, 0.1f);
  physics::ClothDesc invalid = DescFor(open);
  invalid.pressure = 0.1f;
  if (world.CreateCloth(invalid, Mat4::Identity()) != 0) {
    return Fail("open pressure cloth was accepted");
  }
  invalid.pressure = -0.1f;
  if (world.CreateCloth(invalid, Mat4::Identity()) != 0) {
    return Fail("negative pressure cloth was accepted");
  }

  const Vec3 points[] = {{0.12f, 0.12f, 0.12f},
                         {-0.12f, -0.12f, 0.12f},
                         {-0.12f, 0.12f, -0.12f},
                         {0.12f, -0.12f, -0.12f}};
  const u32 indices[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
  physics::ClothDesc desc;
  desc.positions = points;
  desc.vertex_count = 4;
  desc.indices = indices;
  desc.index_count = 12;
  desc.pressure = 0;
  desc.gravity_factor = 0;
  desc.self_collision_distance = 0;
  desc.bend_model = physics::ClothBendModel::kDihedral;
  desc.warp_compliance = 1.0e-3f;
  desc.weft_compliance = 1.0e-3f;
  desc.shear_compliance = 1.0e-3f;
  desc.pressure = 3.0e-2f;
  if (world.CreateCloth(desc, MakeTranslation({0, 1, -1}) * MakeScale(-1)) !=
      0) {
    return Fail("inward reflected pressure-capable mesh was accepted");
  }
  desc.pressure = 0;
  const physics::ClothId body =
      world.CreateCloth(desc, MakeTranslation({0, 1, -1}));
  if (body == 0) return Fail("CreateCloth closed pressure body");
  base::Vector<Vec3> positions;
  if (!ReadCloth(world, body, &positions))
    return Fail("initial pressure body readback");
  f32 initial_radius = 0;
  for (const Vec3& p : positions) initial_radius += Length(p - Vec3{0, 1, -1});
  initial_radius /= static_cast<f32>(positions.size());
  world.SetClothPressure(body, 3.0e-2f);
  for (u32 step = 0; step < 60; ++step) world.Update(kDt);
  if (!ReadCloth(world, body, &positions))
    return Fail("closed pressure body became invalid");
  f32 final_radius = 0;
  for (const Vec3& p : positions) final_radius += Length(p - Vec3{0, 1, -1});
  final_radius /= static_cast<f32>(positions.size());
  if (final_radius <= initial_radius + 1.0e-6f) {
    std::fprintf(stderr, "pressure radius: initial=%g final=%g\n",
                 initial_radius, final_radius);
    return Fail("closed pressure body did not expand");
  }
  world.RemoveCloth(body);
  return 0;
}

int TestSelfCollision(physics::PhysicsWorld& world) {
  const Vec3 points[] = {{-0.25f, -0.2f, 0},     {0.25f, -0.2f, 0},
                         {0, 0.25f, 0},          {-0.25f, -0.2f, 0.003f},
                         {0.25f, -0.2f, 0.003f}, {0, 0.25f, 0.003f}};
  const u32 indices[] = {0, 1, 2, 3, 4, 5};
  physics::ClothDesc desc;
  desc.positions = points;
  desc.vertex_count = 6;
  desc.indices = indices;
  desc.index_count = 6;
  desc.gravity_factor = 0;
  desc.damping = 0;
  desc.bend_model = physics::ClothBendModel::kNone;
  desc.lra_mode = physics::ClothLraMode::kNone;
  desc.collision_radius = 0;
  desc.self_collision_distance = 0.04f;
  desc.self_collision_iterations = 4;
  desc.self_collision_relaxation = 1;
  desc.allow_sleeping = false;
  desc.aerodynamic_drag = 0;
  const physics::ClothId cloth =
      world.CreateCloth(desc, MakeTranslation({0, 1, 1}));
  if (cloth == 0) return Fail("CreateCloth self-collision panels");
  for (u32 step = 0; step < 30; ++step) world.Update(kDt);
  base::Vector<Vec3> positions;
  if (!ReadCloth(world, cloth, &positions))
    return Fail("self-collision panel readback");
  f32 lower = 0, upper = 0;
  for (u32 i = 0; i < 3; ++i) {
    lower += positions[i].z;
    upper += positions[i + 3].z;
  }
  if (std::abs(upper - lower) / 3.0f < 0.025f) {
    return Fail("self-collision did not separate overlapping panels");
  }
  world.RemoveCloth(cloth);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  InitFeatures();
  physics::PhysicsWorld world;
  if (!world.Initialize()) return Fail("physics init (Jolt missing?)");
  if (argc == 2 && std::strcmp(argv[1], "--feature-off") == 0) {
    const Mesh mesh = MakeCurtain(3, 3, 0.1f);
    if (world.CreateCloth(DescFor(mesh), Mat4::Identity()) != 0) {
      return Fail("disabled physics.cloth feature created cloth");
    }
    std::printf("cloth feature gate OK\n");
    return 0;
  }
  auto run = [&](const char* name, auto test) {
    const auto begin = std::chrono::steady_clock::now();
    const int result = test(world);
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin);
    std::printf("%s: %.3f s\n", name, elapsed.count());
    return result;
  };
  if (int rc = run("curtain", TestCurtain)) return rc;
  if (int rc = run("skirt", TestSkirt)) return rc;
  if (int rc = run("skinning", TestSkinning)) return rc;
  if (int rc = run("pressure", TestPressureAndValidation)) return rc;
  if (int rc = run("self collision", TestSelfCollision)) return rc;
  std::printf("cloth_sim_test OK\n");
  return 0;
}
