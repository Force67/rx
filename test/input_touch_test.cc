// input_touch_test: exercises the touch contact state machine (core/input.h)
// that every backend feeds and the HUD reads. The rules that are easy to get
// wrong and impossible to see without a panel are all here: a lifted finger
// must stay visible for exactly one pump so a tap end is never missed, slots
// must compact so a long session does not leak them, per-pump deltas must
// accumulate within a pump and reset across one, and updates for ids we never
// saw (a finger that went down before the window had focus) must be ignored
// rather than corrupt a slot, and a repeated down for an id already on the
// panel must not open a slot nothing can ever release. Needs no window, so it
// runs in the ctest gate.

#include <cmath>
#include <cstdio>

#include "core/input.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

bool Near(rx::f32 a, rx::f32 b) {
  return std::fabs(a - b) < 1e-4f;
}

using rx::TouchState;
using Phase = rx::TouchState::Phase;

// One finger: down, drag, lift. The lift has to survive a pump so a frame that
// only samples state still sees the tap end.
void TestSingleContactLifecycle() {
  std::printf("single contact lifecycle\n");
  TouchState t;

  t.BeginPump();
  t.Apply(7, 100.0f, 200.0f, 1.0f, Phase::kDown);
  Check("down opens one slot", t.count == 1 && t.contacts() == 1);
  Check("down reports a press edge", t.points[0].pressed);
  Check("down is not released", !t.points[0].released);
  Check("position is taken verbatim", Near(t.points[0].x, 100.0f) && Near(t.points[0].y, 200.0f));
  Check("a fresh contact has no delta", Near(t.points[0].dx, 0.0f) && Near(t.points[0].dy, 0.0f));
  Check("active while down", t.active());

  t.BeginPump();
  Check("press edge clears on the next pump", !t.points[0].pressed);
  t.Apply(7, 110.0f, 190.0f, 1.0f, Phase::kMove);
  Check("move updates position", Near(t.points[0].x, 110.0f) && Near(t.points[0].y, 190.0f));
  Check("move reports a signed delta",
        Near(t.points[0].dx, 10.0f) && Near(t.points[0].dy, -10.0f));

  // Two moves in one pump accumulate; the consumer sees the whole travel.
  t.Apply(7, 115.0f, 190.0f, 1.0f, Phase::kMove);
  Check("deltas accumulate within a pump", Near(t.points[0].dx, 15.0f));

  t.BeginPump();
  Check("deltas reset across a pump", Near(t.points[0].dx, 0.0f) && Near(t.points[0].dy, 0.0f));
  Check("position persists across a pump", Near(t.points[0].x, 115.0f));

  t.Apply(7, 115.0f, 190.0f, 0.0f, Phase::kUp);
  Check("lift marks released", t.points[0].released);
  Check("released slot is still visible this pump", t.count == 1);
  Check("released does not count as a contact", t.contacts() == 0);
  Check("not active once lifted", !t.active());

  t.BeginPump();
  Check("released slot is gone next pump", t.count == 0);
}

// Slots must compact, and the survivor must keep its own identity/position.
void TestCompactionKeepsSurvivors() {
  std::printf("compaction keeps survivors\n");
  TouchState t;

  t.BeginPump();
  t.Apply(1, 10.0f, 10.0f, 1.0f, Phase::kDown);
  t.Apply(2, 20.0f, 20.0f, 1.0f, Phase::kDown);
  t.Apply(3, 30.0f, 30.0f, 1.0f, Phase::kDown);
  Check("three contacts", t.count == 3 && t.contacts() == 3);

  // Lift the middle one: the classic index-shuffling bug.
  t.Apply(2, 20.0f, 20.0f, 0.0f, Phase::kUp);
  t.BeginPump();
  Check("two slots remain", t.count == 2);
  Check("survivors keep their ids", t.Find(1) != nullptr && t.Find(3) != nullptr);
  Check("lifted id is gone", t.Find(2) == nullptr);
  Check("survivor keeps its position", Near(t.Find(3)->x, 30.0f));

  // The compacted slot must still track motion, i.e. it was moved not aliased.
  t.Apply(3, 35.0f, 30.0f, 1.0f, Phase::kMove);
  Check("compacted slot still tracks", Near(t.Find(3)->dx, 5.0f));
}

// Events for ids we never saw must not corrupt anything.
void TestUnknownIdsIgnored() {
  std::printf("unknown ids ignored\n");
  TouchState t;

  t.BeginPump();
  t.Apply(99, 50.0f, 50.0f, 1.0f, Phase::kMove);
  Check("move for an unknown id opens no slot", t.count == 0);
  t.Apply(99, 50.0f, 50.0f, 0.0f, Phase::kUp);
  Check("lift for an unknown id opens no slot", t.count == 0);

  t.Apply(1, 10.0f, 10.0f, 1.0f, Phase::kDown);
  t.Apply(99, 999.0f, 999.0f, 1.0f, Phase::kMove);
  Check("unknown move leaves the real contact alone", Near(t.Find(1)->x, 10.0f));
  Check("unknown move adds no slot", t.count == 1);
}

// More fingers than we track must drop cleanly rather than overrun the array.
void TestOverflowDropsCleanly() {
  std::printf("overflow drops cleanly\n");
  TouchState t;

  t.BeginPump();
  for (rx::u32 i = 0; i < TouchState::kMaxPoints + 4; ++i)
    t.Apply(static_cast<rx::i64>(i), static_cast<rx::f32>(i), 0.0f, 1.0f, Phase::kDown);

  Check("count is clamped to capacity", t.count == TouchState::kMaxPoints);
  Check("the first contacts are the ones kept", t.Find(0) != nullptr);
  Check("the overflowing contact was dropped",
        t.Find(static_cast<rx::i64>(TouchState::kMaxPoints) + 3) == nullptr);

  // A dropped finger's lift must not disturb the tracked ones.
  t.Apply(static_cast<rx::i64>(TouchState::kMaxPoints) + 3, 0.0f, 0.0f, 0.0f, Phase::kUp);
  Check("dropped finger's lift is harmless", t.count == TouchState::kMaxPoints);
}

// A canceled gesture reports kUp, so consumers only handle one end state.
void TestCancelEndsContact() {
  std::printf("cancel ends the contact\n");
  TouchState t;

  t.BeginPump();
  t.Apply(4, 10.0f, 10.0f, 1.0f, Phase::kDown);
  t.BeginPump();
  t.Apply(4, 10.0f, 10.0f, 0.0f, Phase::kUp);  // what a canceled finger maps to
  Check("cancel marks released", t.points[0].released);
  Check("cancel zeroes pressure", Near(t.points[0].pressure, 0.0f));
  t.BeginPump();
  Check("cancelled slot is reclaimed", t.count == 0);
}

// A repeated down for a finger already on the panel must not open a second slot
// for the same id. The duplicate would be unreachable (every later move and
// lift resolves to the first slot) and so never released, and BeginPump only
// reclaims released slots, so it would sit there claiming a finger forever.
void TestDuplicateDownDoesNotStrand() {
  std::printf("duplicate down does not strand a contact\n");
  TouchState t;

  t.BeginPump();
  t.Apply(9, 10.0f, 10.0f, 1.0f, Phase::kDown);
  t.Apply(9, 20.0f, 20.0f, 1.0f, Phase::kDown);  // platform repeats itself
  Check("duplicate opens no second slot", t.count == 1 && t.contacts() == 1);
  Check("the slot re-arms at the new position",
        Near(t.points[0].x, 20.0f) && Near(t.points[0].y, 20.0f));
  Check("a re-arm carries no delta", Near(t.points[0].dx, 0.0f) && Near(t.points[0].dy, 0.0f));

  // The one lift the platform will send has to end the contact for good.
  t.Apply(9, 20.0f, 20.0f, 0.0f, Phase::kUp);
  t.BeginPump();
  Check("the lift reclaimed everything", t.count == 0 && !t.active());
}

// A finger that lifts and comes back on the same id inside one pump: the tap
// end still has to be visible, and the new contact must get that pump's motion
// rather than have it applied to the slot that already ended.
void TestRedownWithinAPump() {
  std::printf("re-down within one pump\n");
  TouchState t;

  t.BeginPump();
  t.Apply(5, 10.0f, 10.0f, 1.0f, Phase::kDown);
  t.BeginPump();
  t.Apply(5, 10.0f, 10.0f, 0.0f, Phase::kUp);
  t.Apply(5, 50.0f, 50.0f, 1.0f, Phase::kDown);
  Check("the tap end is still visible", t.count == 2 && t.points[0].released);
  Check("the new contact is live", t.contacts() == 1 && !t.points[1].released);

  t.Apply(5, 60.0f, 50.0f, 1.0f, Phase::kMove);
  Check("motion went to the live contact", Near(t.points[1].x, 60.0f));
  Check("motion left the ended one alone", Near(t.points[0].x, 10.0f));

  t.BeginPump();
  Check("only the live contact survives", t.count == 1 && Near(t.points[0].x, 60.0f));
}

}  // namespace

int main() {
  std::printf("touch contact state machine\n");
  TestSingleContactLifecycle();
  TestCompactionKeepsSurvivors();
  TestUnknownIdsIgnored();
  TestOverflowDropsCleanly();
  TestCancelEndsContact();
  TestDuplicateDownDoesNotStrand();
  TestRedownWithinAPump();

  if (g_failures != 0) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
