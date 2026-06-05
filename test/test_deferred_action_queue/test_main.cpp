// Host-side tests for the DeferredActionQueue primitive (RFC #167, step 1).
//
// The queue formalizes the render-task -> loop-task handoff that ~20 pending*
// flags hand-roll: a callback posts an action from any context; the loop task
// drains it. These tests pin the contract that is the least-tested seam in the
// firmware today (host tests stub the render task entirely): coalescing by
// kind, enum-order priority, stop-on-true drain, and clear-before-act re-arm.
// The DeferredActionGuard is a no-op under UNIT_TEST_HOST, so this is pure.
//
// Run via: pio test -e test_host -f test_deferred_action_queue
#include <unity.h>

#include <cstdint>
#include <vector>

#include "activities/DeferredActionQueue.h"

namespace {
// Enumerator VALUE == drain priority (A drains before B before C).
enum class TestAction : uint8_t { A = 0, B, C, Count };
using Q = crosspoint::DeferredActionQueue<TestAction>;
}  // namespace

void setUp() {}
void tearDown() {}

// ── Coalescing ─────────────────────────────────────────────────────────────

void test_post_twice_drains_once() {
  Q q;
  q.post(TestAction::A);
  q.post(TestAction::A);  // coalesces
  int runs = 0;
  q.drain([&](TestAction a) {
    if (a == TestAction::A) runs++;
    return false;
  });
  TEST_ASSERT_EQUAL_INT(1, runs);
}

// ── Enum-order priority (faithful to the old if-cascade order) ─────────────

void test_drain_visits_in_enum_order_not_post_order() {
  Q q;
  q.post(TestAction::C);  // posted first
  q.post(TestAction::A);  // posted second, but lower enum value
  std::vector<TestAction> order;
  q.drain([&](TestAction a) {
    order.push_back(a);
    return false;
  });
  TEST_ASSERT_EQUAL_size_t(2, order.size());
  TEST_ASSERT_EQUAL_INT((int)TestAction::A, (int)order[0]);  // A before C
  TEST_ASSERT_EQUAL_INT((int)TestAction::C, (int)order[1]);
}

// ── Stop-on-true (the old `return;` after a navigation) ────────────────────

void test_stop_on_true_leaves_later_actions_pending() {
  Q q;
  q.post(TestAction::A);
  q.post(TestAction::C);
  const bool stopped = q.drain([&](TestAction a) {
    return a == TestAction::A;  // stop after A
  });
  TEST_ASSERT_TRUE(stopped);
  TEST_ASSERT_FALSE(q.pending(TestAction::A));  // A was drained
  TEST_ASSERT_TRUE(q.pending(TestAction::C));   // C survived this pass
}

void test_all_false_drains_everything_returns_false() {
  Q q;
  q.post(TestAction::A);
  q.post(TestAction::B);
  int runs = 0;
  const bool stopped = q.drain([&](TestAction) {
    runs++;
    return false;
  });
  TEST_ASSERT_FALSE(stopped);
  TEST_ASSERT_EQUAL_INT(2, runs);
  TEST_ASSERT_FALSE(q.pending(TestAction::A));
  TEST_ASSERT_FALSE(q.pending(TestAction::B));
}

// ── Clear-before-act + re-arm during drain ─────────────────────────────────

void test_bit_cleared_before_handler_runs() {
  Q q;
  q.post(TestAction::A);
  bool clearedDuringRun = false;
  q.drain([&](TestAction a) {
    if (a == TestAction::A) clearedDuringRun = !q.pending(TestAction::A);
    return false;
  });
  TEST_ASSERT_TRUE(clearedDuringRun);  // bit cleared before run() was called
}

void test_repost_self_during_drain_rearms_for_next_pass() {
  Q q;
  q.post(TestAction::A);
  int firstPassRuns = 0;
  q.drain([&](TestAction a) {
    if (a == TestAction::A) {
      firstPassRuns++;
      q.post(TestAction::A);  // re-arm — must NOT loop within this pass
    }
    return false;
  });
  TEST_ASSERT_EQUAL_INT(1, firstPassRuns);     // ran once this pass
  TEST_ASSERT_TRUE(q.pending(TestAction::A));  // re-armed for next pass
  int secondPassRuns = 0;
  q.drain([&](TestAction a) {
    if (a == TestAction::A) secondPassRuns++;
    return false;
  });
  TEST_ASSERT_EQUAL_INT(1, secondPassRuns);
}

// ── pending() / clear() ────────────────────────────────────────────────────

void test_pending_and_clear() {
  Q q;
  TEST_ASSERT_FALSE(q.pending(TestAction::B));
  q.post(TestAction::B);
  TEST_ASSERT_TRUE(q.pending(TestAction::B));
  q.clear(TestAction::B);
  TEST_ASSERT_FALSE(q.pending(TestAction::B));
  int runs = 0;
  q.drain([&](TestAction) {
    runs++;
    return false;
  });
  TEST_ASSERT_EQUAL_INT(0, runs);  // cleared before drain -> nothing runs
}

void test_empty_drain_returns_false() {
  Q q;
  const bool stopped = q.drain([&](TestAction) { return true; });
  TEST_ASSERT_FALSE(stopped);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_post_twice_drains_once);
  RUN_TEST(test_drain_visits_in_enum_order_not_post_order);
  RUN_TEST(test_stop_on_true_leaves_later_actions_pending);
  RUN_TEST(test_all_false_drains_everything_returns_false);
  RUN_TEST(test_bit_cleared_before_handler_runs);
  RUN_TEST(test_repost_self_during_drain_rearms_for_next_pass);
  RUN_TEST(test_pending_and_clear);
  RUN_TEST(test_empty_drain_returns_false);
  return UNITY_END();
}
