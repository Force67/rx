// The mutable third layer: what a save file records on top of an immutable
// bake, and the invariants that let the streamer consult it with a binary
// search while a cell materializes.
#include "world/world_overlay.h"

#include <cstdio>
#include <string>

namespace {

using namespace rx::world;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;
using rx::Quat;
using rx::Vec3;

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

void CheckRejected(bool decoded, const std::string& error, const char* what) {
  if (decoded) {
    std::fprintf(stderr, "FAIL: %s was accepted\n", what);
    ++g_failures;
    return;
  }
  if (error.empty()) {
    std::fprintf(stderr, "FAIL: %s was rejected without a message\n", what);
    ++g_failures;
  }
}

void TestDeltas() {
  WorldOverlay overlay;
  CHECK(overlay.empty());
  CHECK(!overlay.IsDestroyed(5));
  CHECK(overlay.FindMove(5) == nullptr);

  overlay.Destroy(20);
  overlay.Destroy(5);
  overlay.Destroy(20);  // idempotent
  CHECK(overlay.destroyed_count() == 2);
  CHECK(overlay.IsDestroyed(5) && overlay.IsDestroyed(20));
  CHECK(!overlay.IsDestroyed(6));
  // Sorted, which is the invariant IsDestroyed's binary search rests on.
  CHECK(overlay.destroyed()[0] == 5 && overlay.destroyed()[1] == 20);

  overlay.Move(9, {1, 2, 3}, {0, 0, 0, 1}, 2.0f);
  overlay.Move(7, {4, 5, 6}, {0, 0, 0, 1}, 1.0f);
  CHECK(overlay.move_count() == 2);
  CHECK(overlay.moves()[0].stable_id == 7);
  const OverlayMove* move = overlay.FindMove(9);
  CHECK(move != nullptr);
  CHECK(move && move->position.x == 1 && move->scale == 2.0f);

  // Re-moving replaces rather than appends.
  overlay.Move(9, {7, 8, 9}, {0, 0, 0, 1}, 3.0f);
  CHECK(overlay.move_count() == 2);
  CHECK(overlay.FindMove(9)->position.x == 7);

  // Moving something destroyed is refused; destroying something moved drops the
  // move. A row that will not exist has no transform worth keeping.
  overlay.Move(5, {1, 1, 1}, {0, 0, 0, 1}, 1.0f);
  CHECK(overlay.FindMove(5) == nullptr);
  overlay.Destroy(7);
  CHECK(overlay.FindMove(7) == nullptr);
  CHECK(overlay.IsDestroyed(7));

  overlay.Forget(7);
  CHECK(!overlay.IsDestroyed(7));
  overlay.Forget(9);
  CHECK(overlay.FindMove(9) == nullptr);

  overlay.Clear();
  CHECK(overlay.empty());
}

void TestTouchesRange() {
  WorldOverlay overlay;
  CHECK(!overlay.TouchesRange(0, 100));
  overlay.Destroy(150);
  overlay.Move(320, {}, {}, 1.0f);

  CHECK(!overlay.TouchesRange(0, 100));    // [0, 100)
  CHECK(overlay.TouchesRange(100, 100));   // holds 150
  CHECK(!overlay.TouchesRange(200, 100));  // [200, 300)
  CHECK(overlay.TouchesRange(300, 100));   // holds 320
  CHECK(!overlay.TouchesRange(0, 0));      // a cell that owns no ids
  // Exactly on the boundaries.
  CHECK(overlay.TouchesRange(150, 1));
  CHECK(!overlay.TouchesRange(151, 1));
  CHECK(!overlay.TouchesRange(149, 1));
}

void TestRoundTrip() {
  WorldOverlay overlay;
  overlay.Destroy(400);
  overlay.Destroy(100);
  overlay.Move(250, {1, 2, 3}, {0, 0.5f, 0, 0.5f}, 1.5f);
  overlay.Move(50, {-1, -2, -3}, {0, 0, 0, 1}, 0.25f);

  base::Vector<u8> bytes;
  std::string error;
  CHECK(overlay.Encode(&bytes, &error));
  CHECK(error.empty());

  WorldOverlay decoded;
  CHECK(WorldOverlay::Decode(std::span<const u8>(bytes.data(), bytes.size()), &decoded, &error));
  CHECK(decoded.destroyed_count() == 2);
  CHECK(decoded.move_count() == 2);
  CHECK(decoded.IsDestroyed(100) && decoded.IsDestroyed(400));
  const OverlayMove* move = decoded.FindMove(250);
  CHECK(move != nullptr);
  CHECK(move && move->position.y == 2);
  CHECK(move && move->rotation.y == 0.5f);
  CHECK(move && move->scale == 1.5f);
  CHECK(decoded.FindMove(50) != nullptr);

  // An empty overlay round trips too: a save with nothing broken in it.
  WorldOverlay fresh;
  base::Vector<u8> empty_bytes;
  CHECK(fresh.Encode(&empty_bytes, &error));
  WorldOverlay empty_decoded;
  empty_decoded.Destroy(1);
  CHECK(WorldOverlay::Decode(std::span<const u8>(empty_bytes.data(), empty_bytes.size()),
                             &empty_decoded, &error));
  CHECK(empty_decoded.empty());  // Decode replaces, it does not merge
}

void TestRefusesCorruptedBytes() {
  WorldOverlay overlay;
  overlay.Destroy(100);
  overlay.Move(250, {1, 2, 3}, {0, 0, 0, 1}, 1.0f);
  base::Vector<u8> good;
  std::string error;
  CHECK(overlay.Encode(&good, &error));

  WorldOverlay decoded;
  {
    base::Vector<u8> bad(good);
    bad[2] = 'X';
    CheckRejected(WorldOverlay::Decode(std::span<const u8>(bad.data(), bad.size()), &decoded,
                                       &error),
                  error, "a bad magic");
  }
  {
    base::Vector<u8> bad(good);
    bad[bad.size() - 1] ^= 0xff;
    CheckRejected(WorldOverlay::Decode(std::span<const u8>(bad.data(), bad.size()), &decoded,
                                       &error),
                  error, "a flipped body byte");
  }
  {
    base::Vector<u8> bad(good);
    bad.erase(bad.end() - 1);
    CheckRejected(WorldOverlay::Decode(std::span<const u8>(bad.data(), bad.size()), &decoded,
                                       &error),
                  error, "a truncated body");
  }
  {
    base::Vector<u8> bad;
    CheckRejected(WorldOverlay::Decode(std::span<const u8>(bad.data(), bad.size()), &decoded,
                                       &error),
                  error, "an empty file");
  }
  CHECK(WorldOverlay::Decode(std::span<const u8>(good.data(), good.size()), &decoded, &error));
}

// An overlay that arrives out of order would answer IsDestroyed wrongly rather
// than slowly, so the decoder has to catch it. Building one takes hand-written
// bytes: the in-memory API cannot produce it.
void TestRefusesUnsortedFile() {
  WorldOverlay overlay;
  overlay.Destroy(10);
  overlay.Destroy(20);
  base::Vector<u8> bytes;
  std::string error;
  CHECK(overlay.Encode(&bytes, &error));

  // Swap the two ids and repair the checksum so only the ordering is wrong.
  const size_t body = bytes.size() - 16;
  for (u32 i = 0; i < 8; ++i) std::swap(bytes[body + i], bytes[body + 8 + i]);
  u64 checksum = 0xcbf29ce484222325ull;
  for (size_t i = body; i < bytes.size(); ++i) {
    checksum ^= bytes[i];
    checksum *= 0x100000001b3ull;
  }
  for (u32 shift = 0; shift < 64; shift += 8) {
    bytes[body - 8 + shift / 8] = static_cast<u8>(checksum >> shift);
  }

  WorldOverlay decoded;
  CheckRejected(WorldOverlay::Decode(std::span<const u8>(bytes.data(), bytes.size()), &decoded,
                                     &error),
                error, "an unsorted destroyed list");
}

}  // namespace

int main() {
  TestDeltas();
  TestTouchesRange();
  TestRoundTrip();
  TestRefusesCorruptedBytes();
  TestRefusesUnsortedFile();
  if (g_failures) {
    std::fprintf(stderr, "world_overlay_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_overlay_test: ok");
  return 0;
}
