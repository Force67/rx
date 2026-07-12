// Headless acceptance test for the Jolt soft-body strand simulation: builds
// the braid and ponytail test grooms, steps the world with a static and then
// a moving head transform, and asserts the hairstyle constraints hold: nodes
// stay finite, pinned nodes (roots and the ponytail tie) track the transform,
// braid strands stay woven, and no node ends up inside the head sphere.
#include <cmath>
#include <cstdio>

#include <base/containers/vector.h>

#include "core/math.h"
#include "physics/physics_world.h"
#include "render/geometry/hair_groom.h"

using namespace rx;

namespace {

constexpr u32 kPointsPerStrand = render::kGroomPointsPerStrand;

int Fail(const char* what) {
  std::fprintf(stderr, "hair_sim_test FAIL: %s\n", what);
  return 1;
}

physics::PhysicsWorld::StrandGroomDesc ToStrandGroomDesc(const render::GroomData& data) {
  physics::PhysicsWorld::StrandGroomDesc desc;
  desc.points = data.points.data();
  desc.strand_count = data.guide_count;
  desc.points_per_strand = kPointsPerStrand;
  desc.pins = data.pins.data();
  desc.pin_count = static_cast<u32>(data.pins.size() / 2);
  desc.binds = data.binds.data();
  desc.bind_count = static_cast<u32>(data.binds.size() / 4);
  desc.stretch_compliance = data.sim.stretch_compliance;
  desc.bend_compliance = data.sim.bend_compliance;
  desc.bind_compliance = data.sim.bind_compliance;
  desc.damping = data.sim.damping;
  desc.gravity_factor = data.sim.gravity_factor;
  desc.node_mass = data.sim.node_mass;
  desc.node_radius = data.sim.node_radius;
  desc.max_stretch = data.sim.max_stretch;
  desc.iterations = data.sim.iterations;
  return desc;
}

struct Groom {
  render::GroomData data;
  physics::StrandGroomId sim = 0;
  physics::PhysicsWorld::StrandGroomDesc::Sphere head;
  base::Vector<f32> positions;
};

Vec3 NodeAt(const base::Vector<f32>& p, u32 strand, u32 point) {
  size_t i = (static_cast<size_t>(strand) * kPointsPerStrand + point) * 3;
  return {p[i], p[i + 1], p[i + 2]};
}

// Reads back and runs the invariants every groom must satisfy under any
// transform: finite nodes, pinned nodes on their targets, binds at rest
// distance (within tolerance), free nodes outside the head sphere.
int CheckGroom(physics::PhysicsWorld& world, Groom& g, const Mat4& transform,
               const char* name) {
  u32 count = world.StrandGroomPositionCount(g.sim);
  if (count == 0) return Fail("StrandGroomPositionCount");
  g.positions.resize(count);
  if (!world.GetStrandGroomPositions(g.sim, g.positions.data(), count)) {
    return Fail("GetStrandGroomPositions");
  }
  for (u32 i = 0; i < count; ++i) {
    if (!std::isfinite(g.positions[i])) {
      std::fprintf(stderr, "  groom %s\n", name);
      return Fail("non-finite node position");
    }
  }
  // Roots + style pins hold their transformed rest position.
  auto check_pin = [&](u32 strand, u32 point) {
    size_t i = (static_cast<size_t>(strand) * kPointsPerStrand + point) * 3;
    Vec3 rest{g.data.points[i], g.data.points[i + 1], g.data.points[i + 2]};
    Vec3 target = TransformPoint(transform, rest);
    return Length(NodeAt(g.positions, strand, point) - target) < 2e-3f;
  };
  for (u32 s = 0; s < g.data.guide_count; ++s) {
    if (!check_pin(s, 0)) {
      std::fprintf(stderr, "  groom %s strand %u\n", name, s);
      return Fail("root not pinned");
    }
  }
  for (size_t i = 0; i + 1 < g.data.pins.size(); i += 2) {
    if (!check_pin(g.data.pins[i], g.data.pins[i + 1])) {
      std::fprintf(stderr, "  groom %s pin %zu\n", name, i / 2);
      return Fail("style pin not held");
    }
  }
  // Cross-strand binds stay near their rest-pose distance.
  for (size_t i = 0; i + 3 < g.data.binds.size(); i += 4) {
    Vec3 a = NodeAt(g.positions, g.data.binds[i], g.data.binds[i + 1]);
    Vec3 b = NodeAt(g.positions, g.data.binds[i + 2], g.data.binds[i + 3]);
    size_t ia = (static_cast<size_t>(g.data.binds[i]) * kPointsPerStrand + g.data.binds[i + 1]) * 3;
    size_t ib =
        (static_cast<size_t>(g.data.binds[i + 2]) * kPointsPerStrand + g.data.binds[i + 3]) * 3;
    Vec3 ra{g.data.points[ia], g.data.points[ia + 1], g.data.points[ia + 2]};
    Vec3 rb{g.data.points[ib], g.data.points[ib + 1], g.data.points[ib + 2]};
    f32 rest = Length(ra - rb);
    if (Length(a - b) > rest + 0.01f) {
      std::fprintf(stderr, "  groom %s bind %zu: %.4f vs rest %.4f\n", name, i / 4,
                   Length(a - b), rest);
      return Fail("bind did not hold");
    }
  }
  // The head collision sphere keeps the hair off the scalp (small tolerance
  // for the solver pushing against gravity).
  Vec3 head_center = TransformPoint(transform, g.head.center);
  for (u32 s = 0; s < g.data.guide_count; ++s) {
    for (u32 k = 1; k < kPointsPerStrand; ++k) {
      f32 d = Length(NodeAt(g.positions, s, k) - head_center);
      if (d < g.head.radius * 0.85f) {
        std::fprintf(stderr, "  groom %s strand %u node %u: %.4f into r=%.4f\n", name, s, k, d,
                     g.head.radius);
        return Fail("node inside the head sphere");
      }
    }
  }
  return 0;
}

}  // namespace

int main() {
  physics::PhysicsWorld world;
  if (!world.Initialize()) return Fail("physics init (Jolt missing?)");

  const Mat4 rest_transform = MakeTranslation({0, 1.6f, 0});
  Groom braid, ponytail;
  if (!render::BuildTestGroom(render::TestGroomStyle::kBraid, 48, 1, &braid.data)) {
    return Fail("BuildTestGroom braid");
  }
  if (!render::BuildTestGroom(render::TestGroomStyle::kPonytail, 120, 2, &ponytail.data)) {
    return Fail("BuildTestGroom ponytail");
  }
  if (braid.data.binds.empty()) return Fail("braid groom has no binds");
  if (ponytail.data.pins.empty()) return Fail("ponytail groom has no pins");

  for (Groom* g : {&braid, &ponytail}) {
    g->head = {g->data.collision_center, g->data.collision_radius};
    physics::PhysicsWorld::StrandGroomDesc desc = ToStrandGroomDesc(g->data);
    desc.spheres = &g->head;
    desc.sphere_count = 1;
    g->sim = world.CreateStrandGroom(desc, rest_transform);
    if (g->sim == 0) return Fail("CreateStrandGroom");
  }

  // Settle under gravity with a static head.
  const f32 dt = 1.0f / 60.0f;
  for (int i = 0; i < 240; ++i) {
    world.SetStrandGroomTransform(braid.sim, rest_transform, dt);
    world.SetStrandGroomTransform(ponytail.sim, rest_transform, dt);
    world.Update(dt);
  }
  if (int rc = CheckGroom(world, braid, rest_transform, "braid (settled)")) return rc;
  if (int rc = CheckGroom(world, ponytail, rest_transform, "ponytail (settled)")) return rc;

  // Gravity acts: the braid tip must hang below its root.
  {
    Vec3 root = NodeAt(braid.positions, 0, 0);
    Vec3 tip = NodeAt(braid.positions, 0, kPointsPerStrand - 1);
    std::printf("braid: root y=%.3f tip y=%.3f\n", root.y, tip.y);
    if (tip.y > root.y - 0.15f) return Fail("braid does not hang");
  }

  // Swing the heads (turn + bob, the demo's orbit) and re-check every
  // invariant against the moving transform each step.
  Mat4 moving = rest_transform;
  for (int i = 1; i <= 240; ++i) {
    f32 a = static_cast<f32>(i) * dt * 3.0f;
    moving = MakeTranslation({0.05f * std::sin(a), 1.6f + 0.03f * std::sin(a * 1.7f), 0}) *
             MakeFromQuat(QuatFromAxisAngle({0, 1, 0}, 0.8f * std::sin(a)));
    world.SetStrandGroomTransform(braid.sim, moving, dt);
    world.SetStrandGroomTransform(ponytail.sim, moving, dt);
    world.Update(dt);
  }
  // One settling step so the pins finish converging on the final pose.
  for (int i = 0; i < 60; ++i) {
    world.SetStrandGroomTransform(braid.sim, moving, dt);
    world.SetStrandGroomTransform(ponytail.sim, moving, dt);
    world.Update(dt);
  }
  if (int rc = CheckGroom(world, braid, moving, "braid (moved)")) return rc;
  if (int rc = CheckGroom(world, ponytail, moving, "ponytail (moved)")) return rc;

  world.RemoveStrandGroom(braid.sim);
  world.RemoveStrandGroom(ponytail.sim);
  std::printf("hair_sim_test OK\n");
  return 0;
}
