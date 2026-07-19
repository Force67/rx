#include "render/geometry/procedural_grass.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition)
    return;
  std::fprintf(stderr, "procedural_grass_test: FAIL: %s\n", message);
  ++failures;
}

bool Near(rx::f32 a, rx::f32 b) {
  return std::fabs(a - b) < 1e-5f;
}

void SetPoint(rx::f32 point[3], rx::f32 x, rx::f32 y, rx::f32 z) {
  point[0] = x;
  point[1] = y;
  point[2] = z;
}

}  // namespace

int main() {
  using rx::render::GrassGenerationSettings;
  using rx::render::GrassSurfaceCandidateCount;
  using rx::render::GrassSurfaceTriangle;
  using rx::render::ProceduralGrass;
  using rx::render::SanitizeGrassSettings;

  GrassGenerationSettings unsafe;
  unsafe.candidate_spacing = -2.0f;
  unsafe.stream_tile_size = 0.0f;
  unsafe.stream_radius = 900.0f;
  unsafe.density_lod_start = -4.0f;
  unsafe.density_lod_end = -8.0f;
  unsafe.far_density = 2.0f;
  unsafe.geometry_lod_start = 3.0f;
  unsafe.geometry_lod_end = 1.0f;
  unsafe.fade_start = 2.0f;
  unsafe.fade_end = 1.0f;
  unsafe.max_slope_cos = -1.0f;
  unsafe.bend_recovery_time = -4.0f;
  unsafe.max_blades = 0;
  const GrassGenerationSettings safe = SanitizeGrassSettings(unsafe);
  Check(Near(safe.candidate_spacing, 0.08f), "candidate spacing has a safe minimum");
  Check(Near(safe.stream_tile_size, 1.0f), "stream tile has a safe minimum");
  Check(Near(safe.stream_radius, 512.0f), "stream radius is capacity bounded");
  Check(Near(safe.density_lod_start, 0.0f), "density LOD starts at nonnegative distance");
  Check(safe.density_lod_end > safe.density_lod_start,
        "density LOD range remains ordered");
  Check(Near(safe.far_density, 1.0f), "far density is normalized");
  Check(safe.geometry_lod_end > safe.geometry_lod_start,
        "geometry LOD range remains ordered");
  Check(safe.fade_end > safe.fade_start, "fade range remains ordered");
  Check(Near(safe.max_slope_cos, 0.0f), "slope threshold is normalized");
  Check(Near(safe.bend_recovery_time, 0.0f), "bend recovery can be disabled");
  Check(safe.max_blades == 1, "blade capacity has a nonzero minimum");

  GrassGenerationSettings non_finite;
  non_finite.candidate_spacing = std::numeric_limits<rx::f32>::quiet_NaN();
  non_finite.stream_radius = std::numeric_limits<rx::f32>::infinity();
  non_finite.fade_end = std::numeric_limits<rx::f32>::quiet_NaN();
  non_finite.bend_recovery_time = std::numeric_limits<rx::f32>::infinity();
  const GrassGenerationSettings finite = SanitizeGrassSettings(non_finite);
  Check(std::isfinite(finite.candidate_spacing), "non-finite spacing uses a default");
  Check(std::isfinite(finite.stream_radius), "non-finite radius uses a default");
  Check(finite.fade_end <= finite.stream_radius,
        "fade completes inside the stream radius");
  Check(std::isfinite(finite.bend_recovery_time),
        "non-finite bend recovery uses a default");

  GrassSurfaceTriangle triangle;
  SetPoint(triangle.p0, 0.0f, 0.0f, 0.0f);
  SetPoint(triangle.p1, 2.0f, 0.0f, 0.0f);
  SetPoint(triangle.p2, 0.0f, 0.0f, 2.0f);
  Check(GrassSurfaceCandidateCount(triangle, 0.5f) == 8,
        "triangle area maps to the expected candidate count");
  Check(GrassSurfaceCandidateCount(triangle, 100.0f) == 1,
        "nondegenerate triangles retain at least one candidate");

  SetPoint(triangle.p1, 1e9f, 0.0f, 0.0f);
  SetPoint(triangle.p2, 0.0f, 0.0f, 1e9f);
  Check(
      GrassSurfaceCandidateCount(triangle, 0.08f) == std::numeric_limits<rx::u32>::max(),
      "oversized candidate counts clamp before integer conversion");

  SetPoint(triangle.p1, 2.0f, 0.0f, 0.0f);
  SetPoint(triangle.p2, 4.0f, 0.0f, 0.0f);
  Check(GrassSurfaceCandidateCount(triangle, 0.5f) == 0,
        "degenerate triangles are rejected");
  triangle.p2[0] = std::numeric_limits<rx::f32>::infinity();
  Check(GrassSurfaceCandidateCount(triangle, 0.5f) == 0,
        "non-finite triangles are rejected");

  static_assert(ProceduralGrass::kMaxCandidates == 1u << 20);
  static_assert(ProceduralGrass::kMaxBlades == 1u << 18);
  static_assert(ProceduralGrass::kVerticesPerBlade == 42);
  static_assert(ProceduralGrass::kFarVerticesPerBlade == 18);
  static_assert(ProceduralGrass::kBendResolution == 512);
  static_assert(ProceduralGrass::kInstanceStride == 72);

  if (failures != 0)
    return 1;
  std::printf("procedural_grass_test: PASS\n");
  return 0;
}
