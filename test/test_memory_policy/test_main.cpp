// Host-side tests for the MemoryPolicy deep module (RFC #163, step 1).
//
// Exercises the pure, host-testable core: named heap gates (canAfford),
// the OOM recovery-ladder decision (nextRecoveryStep), the dynamic
// probe-before-grow with one-shot shed (roomToGrow), and the shed-evictor
// registry. The probe sources are the HeapGuard largest-block override plus
// the MemoryPolicy total-free override sibling.
//
// Run via: pio test -e test_host -f test_memory_policy
#include <unity.h>

#include <cstddef>
#include <cstdint>

#include "HeapGuard.h"
#include "MemoryPolicy.h"

using crosspoint::heap::clearLargestFreeBlockOverride;
using crosspoint::heap::setLargestFreeBlockOverride;

using crosspoint::mem::canAfford;
using crosspoint::mem::clearShedEvictors;
using crosspoint::mem::clearTotalFreeOverride;
using crosspoint::mem::nextRecoveryStep;
using crosspoint::mem::Op;
using crosspoint::mem::Recovery;
using crosspoint::mem::RecoveryContext;
using crosspoint::mem::registerShedEvictor;
using crosspoint::mem::roomToGrow;
using crosspoint::mem::setTotalFreeOverride;
using crosspoint::mem::shedUnderPressure;

void setUp() {
  clearLargestFreeBlockOverride();
  clearTotalFreeOverride();
  clearShedEvictors();
}
void tearDown() {
  clearLargestFreeBlockOverride();
  clearTotalFreeOverride();
  clearShedEvictors();
}

// ── Named gates: boundary + metric ─────────────────────────────────────────

void test_render_task_gate_boundary_at_12k() {
  setLargestFreeBlockOverride(12 * 1024 - 1);
  TEST_ASSERT_FALSE(canAfford(Op::SpawnRenderTask));
  setLargestFreeBlockOverride(12 * 1024);
  TEST_ASSERT_TRUE(canAfford(Op::SpawnRenderTask));
}

void test_layout_gate_boundary_at_48k() {
  setLargestFreeBlockOverride(48 * 1024 - 1);
  TEST_ASSERT_FALSE(canAfford(Op::BuildSectionLayout));
  setLargestFreeBlockOverride(48 * 1024);
  TEST_ASSERT_TRUE(canAfford(Op::BuildSectionLayout));
}

void test_sleep_playlist_gate_boundary_at_64k() {
  setLargestFreeBlockOverride(64 * 1024 - 1);
  TEST_ASSERT_FALSE(canAfford(Op::ScanSleepPlaylist));
  setLargestFreeBlockOverride(64 * 1024);
  TEST_ASSERT_TRUE(canAfford(Op::ScanSleepPlaylist));
}

void test_rich_sleep_reads_total_free_not_largest_block() {
  // Big contiguous block but low total free: the metric trap. RenderRichSleep
  // gates on TOTAL free (30 KB), ScanSleepPlaylist on LARGEST block (64 KB).
  setLargestFreeBlockOverride(90 * 1024);
  setTotalFreeOverride(20 * 1024);
  TEST_ASSERT_FALSE(canAfford(Op::RenderRichSleepScreen));  // 20K total < 30K
  TEST_ASSERT_TRUE(canAfford(Op::ScanSleepPlaylist));       // 90K largest >= 64K

  // Inverse: plenty of total free but fragmented into small blocks.
  setLargestFreeBlockOverride(10 * 1024);
  setTotalFreeOverride(120 * 1024);
  TEST_ASSERT_TRUE(canAfford(Op::RenderRichSleepScreen));  // 120K total >= 30K
  TEST_ASSERT_FALSE(canAfford(Op::ScanSleepPlaylist));     // 10K largest < 64K
}

// ── Recovery ladder (pure decision) ────────────────────────────────────────

void test_ladder_releases_anchor_first() {
  RecoveryContext ctx{};
  ctx.anchorHeld = true;
  ctx.maxAlreadyDropped = false;
  ctx.largestAfterStep = 30 * 1024;
  TEST_ASSERT_EQUAL(Recovery::ReleaseAnchor, nextRecoveryStep(ctx));
}

void test_ladder_drops_max_resources_after_anchor() {
  RecoveryContext ctx{};
  ctx.anchorHeld = false;
  ctx.maxAlreadyDropped = false;
  ctx.largestAfterStep = 30 * 1024;
  TEST_ASSERT_EQUAL(Recovery::ReleaseMaxResources, nextRecoveryStep(ctx));
}

void test_ladder_silent_restart_below_hard_floor_with_budget() {
  RecoveryContext ctx{};
  ctx.anchorHeld = false;
  ctx.maxAlreadyDropped = true;
  ctx.largestAfterStep = 15 * 1024;  // < 20K hard floor
  ctx.bookOpen = true;
  ctx.restartBudget = 2;
  TEST_ASSERT_EQUAL(Recovery::SilentRestart, nextRecoveryStep(ctx));
}

void test_ladder_gives_up_below_hard_floor_when_budget_exhausted() {
  RecoveryContext ctx{};
  ctx.anchorHeld = false;
  ctx.maxAlreadyDropped = true;
  ctx.largestAfterStep = 15 * 1024;
  ctx.bookOpen = true;
  ctx.restartBudget = 0;  // exhausted
  TEST_ASSERT_EQUAL(Recovery::GiveUp, nextRecoveryStep(ctx));
}

void test_ladder_gives_up_below_hard_floor_when_no_book_open() {
  RecoveryContext ctx{};
  ctx.anchorHeld = false;
  ctx.maxAlreadyDropped = true;
  ctx.largestAfterStep = 15 * 1024;
  ctx.bookOpen = false;  // silent-restart-to-reader needs an open book
  ctx.restartBudget = 2;
  TEST_ASSERT_EQUAL(Recovery::GiveUp, nextRecoveryStep(ctx));
}

void test_ladder_proceeds_optimistically_above_hard_floor() {
  // Faithful to current heapHeadroomOkForLayout: after dropping caches, if
  // largest is >= the 20K hard floor (even if still < the 48K gate), attempt
  // layout instead of restarting.
  RecoveryContext ctx{};
  ctx.anchorHeld = false;
  ctx.maxAlreadyDropped = true;
  ctx.largestAfterStep = 30 * 1024;  // >= 20K floor, < 48K gate
  ctx.bookOpen = true;
  ctx.restartBudget = 2;
  TEST_ASSERT_EQUAL(Recovery::Proceed, nextRecoveryStep(ctx));
}

// ── Dynamic probe-before-grow + shed ───────────────────────────────────────

void test_room_to_grow_passes_without_shedding_when_heap_ample() {
  setLargestFreeBlockOverride(50 * 1024);
  static int shedCalls;
  shedCalls = 0;
  registerShedEvictor([]() { shedCalls++; });
  TEST_ASSERT_TRUE(roomToGrow(20 * 1024));
  TEST_ASSERT_EQUAL_INT(0, shedCalls);  // ample heap -> evictor untouched
}

void test_room_to_grow_sheds_once_then_succeeds() {
  setLargestFreeBlockOverride(5 * 1024);  // too small for a 20K grow
  static int shedCalls;
  shedCalls = 0;
  // The fake evictor "frees" memory by raising the override.
  registerShedEvictor([]() {
    shedCalls++;
    setLargestFreeBlockOverride(50 * 1024);
  });
  TEST_ASSERT_TRUE(roomToGrow(20 * 1024));  // flips false -> shed -> true
  TEST_ASSERT_EQUAL_INT(1, shedCalls);      // shed exactly once
}

void test_room_to_grow_fails_when_shed_cannot_free_enough() {
  setLargestFreeBlockOverride(5 * 1024);
  static int shedCalls;
  shedCalls = 0;
  registerShedEvictor([]() { shedCalls++; });  // frees nothing
  TEST_ASSERT_FALSE(roomToGrow(20 * 1024));
  TEST_ASSERT_EQUAL_INT(1, shedCalls);  // attempted once, gave up
}

void test_shed_under_pressure_runs_all_registered_evictors() {
  static int a;
  static int b;
  a = 0;
  b = 0;
  registerShedEvictor([]() { a++; });
  registerShedEvictor([]() { b++; });
  shedUnderPressure();
  TEST_ASSERT_EQUAL_INT(1, a);
  TEST_ASSERT_EQUAL_INT(1, b);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_task_gate_boundary_at_12k);
  RUN_TEST(test_layout_gate_boundary_at_48k);
  RUN_TEST(test_sleep_playlist_gate_boundary_at_64k);
  RUN_TEST(test_rich_sleep_reads_total_free_not_largest_block);
  RUN_TEST(test_ladder_releases_anchor_first);
  RUN_TEST(test_ladder_drops_max_resources_after_anchor);
  RUN_TEST(test_ladder_silent_restart_below_hard_floor_with_budget);
  RUN_TEST(test_ladder_gives_up_below_hard_floor_when_budget_exhausted);
  RUN_TEST(test_ladder_gives_up_below_hard_floor_when_no_book_open);
  RUN_TEST(test_ladder_proceeds_optimistically_above_hard_floor);
  RUN_TEST(test_room_to_grow_passes_without_shedding_when_heap_ample);
  RUN_TEST(test_room_to_grow_sheds_once_then_succeeds);
  RUN_TEST(test_room_to_grow_fails_when_shed_cannot_free_enough);
  RUN_TEST(test_shed_under_pressure_runs_all_registered_evictors);
  return UNITY_END();
}
