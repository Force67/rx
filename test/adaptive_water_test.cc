#include "render/geometry/adaptive_water.h"

#include <cstdio>

using rx::render::AdaptiveWaterMesh;

int main() {
  static_assert(AdaptiveWaterMesh::kMaxTriangles == 32768);
  static_assert(AdaptiveWaterMesh::SanitizeBudget(0) == 2);
  static_assert(AdaptiveWaterMesh::SanitizeBudget(8192) == 8192);
  static_assert(AdaptiveWaterMesh::SanitizeBudget(~rx::u32{0}) == AdaptiveWaterMesh::kMaxTriangles);
  std::printf("adaptive_water_test: PASS (capacity %u triangles)\n",
              AdaptiveWaterMesh::kMaxTriangles);
  return 0;
}
