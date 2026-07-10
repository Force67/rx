#include "anim/foot_placement.h"

#include <vector>

#include "anim/anim_internal.h"

namespace rx::anim {

using detail::GraphState;

struct FootPlacement::Impl {
  const GraphState* graph = nullptr;
  kinema::FootLimb feet[2];
  u32 pelvis = 0;
  bool ok = false;
  f32 ankle_height = 0.08f;
  // Model-space scratch (kinema fills it via LocalToModel); sized once at Bind.
  std::vector<kinema::Vec3> mt;
  std::vector<kinema::Quat> mr;
  std::vector<f32> ms;

  kinema::PoseView model() {
    return kinema::PoseView{mt.data(), mr.data(), ms.data(), static_cast<u32>(mt.size())};
  }
};

FootPlacement::FootPlacement() : impl_(std::make_unique<Impl>()) {}
FootPlacement::~FootPlacement() = default;
FootPlacement::FootPlacement(FootPlacement&&) noexcept = default;
FootPlacement& FootPlacement::operator=(FootPlacement&&) noexcept = default;

bool FootPlacement::Bind(const AnimGraph& graph, f32 ankle_height) {
  const GraphState* g = graph.state();
  Impl& d = *impl_;
  d.graph = g;
  d.ankle_height = ankle_height;
  d.ok = false;
  if (!g) return false;
  const kinema::Skeleton& sk = g->skeleton;
  auto f = [&](const char* n) { return sk.Find(kinema::HashName(n)); };
  int lh = f("NPC L Thigh [LThg]"), lk = f("NPC L Calf [LClf]"), la = f("NPC L Foot [Lft ]");
  int rh = f("NPC R Thigh [RThg]"), rk = f("NPC R Calf [RClf]"), ra = f("NPC R Foot [Rft ]");
  int pv = f("NPC Pelvis [Pelv]");
  if (pv < 0) pv = f("NPC Root [Root]");
  if (lh < 0 || lk < 0 || la < 0 || rh < 0 || rk < 0 || ra < 0 || pv < 0) return false;
  d.feet[0] = kinema::FootLimb{static_cast<u32>(lh), static_cast<u32>(lk), static_cast<u32>(la)};
  d.feet[1] = kinema::FootLimb{static_cast<u32>(rh), static_cast<u32>(rk), static_cast<u32>(ra)};
  d.pelvis = static_cast<u32>(pv);
  const u32 n = sk.count();
  d.mt.assign(n, kinema::Vec3{});
  d.mr.assign(n, kinema::Quat{});
  d.ms.assign(n, 1.0f);
  d.ok = true;
  return true;
}

bool FootPlacement::bound() const { return impl_->ok; }

f32 FootPlacement::Apply(SkeletonPose* pose, const GroundProbe& probe) {
  Impl& d = *impl_;
  if (!d.ok) return 0.0f;
  const kinema::Skeleton& sk = d.graph->skeleton;
  const Vec3 up{0, 1, 0};
  kinema::PoseView local = detail::AsKinema(*pose);

  // Ankle model positions to cast from. Start the ray well above each foot so a
  // raised swing foot still probes the ground beneath it.
  kinema::LocalToModel(sk, local, d.model());
  kinema::FootHit hits[2];
  for (int i = 0; i < 2; ++i) {
    const kinema::Vec3& a = d.mt[d.feet[i].ankle];
    Vec3 origin{a.x, a.y + 0.5f, a.z};
    Vec3 hit{}, normal{0, 1, 0};
    hits[i].valid = probe(origin, &hit, &normal);
    hits[i].point = kinema::Vec3{hit.x, hit.y, hit.z};
    hits[i].normal = kinema::Vec3{normal.x, normal.y, normal.z};
  }

  kinema::FootPlacementSolve solve;
  solve.pelvis = d.pelvis;
  solve.feet = d.feet;
  solve.hits = hits;
  solve.foot_count = 2;
  solve.up = kinema::Vec3{up.x, up.y, up.z};
  solve.ankle_height = d.ankle_height;
  solve.max_drop = 0.5f;
  solve.soft = 0.03f;
  solve.weight = 1.0f;
  return kinema::SolveFootPlacement(sk, local, d.model(), solve);
}

}  // namespace rx::anim
