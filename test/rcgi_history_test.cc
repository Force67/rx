#include "render/gi/rcgi_history.h"

#include <cstdio>

using namespace rx;
using namespace rx::render;

namespace {

int failures = 0;
#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);          \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

RcgiHistoryLighting Interior(bool miss = true) {
  return {.authored_interior = true,
          .interior_miss = miss,
          .interior_ambient = {0.1f, 0.2f, 0.3f},
          .sun_direction = {0, -1, 0},
          .sun_intensity = 0.5f,
          .sun_color = {0.8f, 0.7f, 0.6f}};
}

}  // namespace

int main() {
  RcgiCascadeValidity first = ComputeRcgiCascadeValidity(0u, 2u, true);
  CHECK(first.before_blend == 0u);
  CHECK(first.after_blend == 4u);

  RcgiCascadeValidity reset = ComputeRcgiCascadeValidity(0xfu, 2u, true);
  CHECK(reset.before_blend == 0xbu);
  CHECK(reset.after_blend == 0xfu);

  RcgiCascadeValidity steady = ComputeRcgiCascadeValidity(0xfu, 2u, false);
  CHECK(steady.before_blend == 0xfu);
  CHECK(steady.after_blend == 0xfu);

  RcgiHistoryLighting outdoor;
  RcgiHistoryLighting interior_no_miss = Interior(false);
  CHECK(ShouldInvalidateRcgiHistory(outdoor, interior_no_miss));
  CHECK(ShouldInvalidateRcgiHistory(interior_no_miss, outdoor));
  CHECK(!ShouldInvalidateRcgiHistory(interior_no_miss, interior_no_miss));

  RcgiHistoryLighting changed = interior_no_miss;
  changed.sun_intensity = 0.75f;
  CHECK(ShouldInvalidateRcgiHistory(interior_no_miss, changed));
  changed = interior_no_miss;
  changed.sun_direction.x = 0.2f;
  CHECK(ShouldInvalidateRcgiHistory(interior_no_miss, changed));
  changed = interior_no_miss;
  changed.sun_color.z = 0.9f;
  CHECK(ShouldInvalidateRcgiHistory(interior_no_miss, changed));

  RcgiHistoryLighting interior = Interior();
  changed = interior;
  changed.interior_ambient.x = 0.4f;
  CHECK(ShouldInvalidateRcgiHistory(interior, changed));
  CHECK(ShouldInvalidateRcgiHistory(interior_no_miss, interior));

  RcgiHistoryLighting dormant_a;
  dormant_a.interior_ambient = {1, 0, 0};
  RcgiHistoryLighting dormant_b = dormant_a;
  dormant_b.interior_ambient = {0, 1, 0};
  CHECK(!ShouldInvalidateRcgiHistory(dormant_a, dormant_b));

  if (failures == 0) std::printf("rcgi_history_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
