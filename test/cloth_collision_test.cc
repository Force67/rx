#include "physics/cloth_collision.h"

#include <base/containers/vector.h>

#include <cmath>
#include <cstdio>

using namespace rx;

namespace {

int Fail(const char* what) {
  std::fprintf(stderr, "cloth_collision_test FAIL: %s\n", what);
  return 1;
}

physics::detail::ClothTopology ManualTopology(u32 vertex_count) {
  physics::detail::ClothTopology topology;
  topology.neighbor_offsets.resize(static_cast<size_t>(vertex_count) + 1);
  return topology;
}

bool Finite(const base::Vector<Vec3>& values) {
  for (const Vec3& value : values) {
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z)) {
      return false;
    }
  }
  return true;
}

int TestTopology() {
  const Vec3 bow_tie[] = {
      {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}};
  const u32 bow_tie_indices[] = {0, 1, 2, 0, 3, 4};
  physics::detail::ClothTopology topology;
  if (physics::detail::BuildClothTopology(bow_tie, 5, bow_tie_indices, 6,
                                          &topology)) {
    return Fail("bow-tie vertex topology was accepted");
  }

  const Vec3 isolated[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {2, 2, 2}};
  const u32 triangle[] = {0, 1, 2};
  if (physics::detail::BuildClothTopology(isolated, 4, triangle, 3,
                                          &topology)) {
    return Fail("isolated vertex topology was accepted");
  }

  const Vec3 overflow[] = {{3.0e38f, 0, 0}, {-3.0e38f, 0, 0}, {0, 3.0e38f, 0}};
  if (physics::detail::BuildClothTopology(overflow, 3, triangle, 3,
                                          &topology)) {
    return Fail("finite geometry with overflowing metrics was accepted");
  }

  const Vec3 tetra[] = {{0.12f, 0.12f, 0.12f},
                        {-0.12f, -0.12f, 0.12f},
                        {-0.12f, 0.12f, -0.12f},
                        {0.12f, -0.12f, -0.12f}};
  const u32 tetra_indices[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
  if (!physics::detail::BuildClothTopology(tetra, 4, tetra_indices, 12,
                                           &topology) ||
      !topology.closed || topology.component_count != 1 ||
      topology.signed_volume <= 0) {
    return Fail(
        "closed outward tetra topology was not classified for pressure");
  }
  return 0;
}

int TestVertexFaceCcd() {
  physics::detail::ClothTopology topology = ManualTopology(4);
  topology.indices = {0, 1, 2};
  base::Vector<Vec3> positions{
      {-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0, 0.5f, 0}, {0, 0, 0.1f}};
  base::Vector<Vec3> velocities(4);
  velocities[3] = {0, 0, -12};
  base::Vector<f32> inverse_masses{0, 0, 0, 1};
  physics::detail::ClothSelfCollisionConfig config;
  config.distance = 0.02f;
  config.iterations = 2;
  physics::detail::ClothSelfCollisionScratch scratch;
  if (physics::detail::SolveClothSelfCollision(topology, config, positions,
                                               &velocities, inverse_masses,
                                               1.0f / 60.0f, &scratch) == 0) {
    return Fail("swept vertex/triangle crossing produced no contact");
  }
  const Vec3 predicted = positions[3] + velocities[3] * (1.0f / 60.0f);
  if (!Finite(velocities) || predicted.z < 0.015f) {
    return Fail("vertex crossed through triangle");
  }
  return 0;
}

int TestHighTangentialCcd() {
  physics::detail::ClothTopology topology = ManualTopology(4);
  topology.indices = {0, 1, 2};
  base::Vector<Vec3> positions{
      {-10, -10, 0}, {20, -10, 0}, {5, 20, 0}, {0, 0, 0.1f}};
  base::Vector<Vec3> velocities(4);
  velocities[3] = {600, 0, -12};
  base::Vector<f32> inverse_masses{0, 0, 0, 1};
  physics::detail::ClothSelfCollisionConfig config;
  config.distance = 0.005f;
  config.max_velocity = 1000;
  physics::detail::ClothSelfCollisionScratch scratch;
  if (physics::detail::SolveClothSelfCollision(topology, config, positions,
                                               &velocities, inverse_masses,
                                               1.0f / 60.0f, &scratch) == 0) {
    return Fail("high-tangential-speed crossing produced no contact");
  }
  const Vec3 predicted = positions[3] + velocities[3] * (1.0f / 60.0f);
  return predicted.z >= 0.004f
             ? 0
             : Fail("high-tangential-speed vertex crossed triangle");
}

int TestEdgeEdgeCcd() {
  physics::detail::ClothTopology topology = ManualTopology(4);
  topology.edges = {0, 1, 2, 3};
  base::Vector<Vec3> positions{
      {-0.5f, 0, 0}, {0.5f, 0, 0}, {0, -0.5f, 0.1f}, {0, 0.5f, 0.1f}};
  base::Vector<Vec3> velocities(4);
  velocities[2] = velocities[3] = {0, 0, -12};
  base::Vector<f32> inverse_masses{0, 0, 1, 1};
  physics::detail::ClothSelfCollisionConfig config;
  config.distance = 0.02f;
  config.iterations = 2;
  physics::detail::ClothSelfCollisionScratch scratch;
  if (physics::detail::SolveClothSelfCollision(topology, config, positions,
                                               &velocities, inverse_masses,
                                               1.0f / 60.0f, &scratch) == 0) {
    return Fail("swept edge/edge crossing produced no contact");
  }
  const f32 z2 = positions[2].z + velocities[2].z / 60.0f;
  const f32 z3 = positions[3].z + velocities[3].z / 60.0f;
  if (!Finite(velocities) || std::min(z2, z3) < 0.015f) {
    return Fail("edge crossed through edge");
  }
  return 0;
}

int TestRuntimeDegenerateTriangle() {
  physics::detail::ClothTopology topology = ManualTopology(4);
  topology.indices = {0, 1, 2};
  base::Vector<Vec3> positions{
      {-0.5f, 0, 0}, {0, 0, 0}, {0.5f, 0, 0}, {0, 0, 0.005f}};
  base::Vector<Vec3> velocities(4);
  base::Vector<f32> inverse_masses{0, 0, 0, 1};
  physics::detail::ClothSelfCollisionConfig config;
  config.distance = 0.02f;
  physics::detail::ClothSelfCollisionScratch scratch;
  physics::detail::SolveClothSelfCollision(topology, config, positions,
                                           &velocities, inverse_masses,
                                           1.0f / 60.0f, &scratch);
  return Finite(velocities)
             ? 0
             : Fail("collapsed runtime triangle generated non-finite velocity");
}

}  // namespace

int main() {
  if (int rc = TestTopology()) return rc;
  if (int rc = TestVertexFaceCcd()) return rc;
  if (int rc = TestHighTangentialCcd()) return rc;
  if (int rc = TestEdgeEdgeCcd()) return rc;
  if (int rc = TestRuntimeDegenerateTriangle()) return rc;
  std::printf("cloth_collision_test OK\n");
  return 0;
}
