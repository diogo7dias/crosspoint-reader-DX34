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

// ── Recovery ladder EXECUTION loop (runRecoveryLadder) ──────────────────────
// These drive the whole loop (re-probe + flag flips + gate re-check + budget
// branch) that used to live untested inside EpubReaderActivity, using a fake
// that scripts the heap through the same setLargestFreeBlockOverride seam.

namespace {
struct LoopFake {
  int anchorCalls = 0;
  int maxCalls = 0;
  int restartCalls = 0;
  size_t afterAnchor = 0;        // largest the heap "reveals" after releaseAnchor
  size_t afterMax = 0;           // largest after releaseMaxResources
  bool restartTriggers = false;  // trySilentRestart return (false = budget lost)
};
size_t loopFakeAnchor(void* p) {
  auto* f = static_cast<LoopFake*>(p);
  f->anchorCalls++;
  setLargestFreeBlockOverride(f->afterAnchor);
  return f->afterAnchor;
}
size_t loopFakeMax(void* p) {
  auto* f = static_cast<LoopFake*>(p);
  f->maxCalls++;
  setLargestFreeBlockOverride(f->afterMax);
  return f->afterMax;
}
bool loopFakeRestart(void* p) {
  auto* f = static_cast<LoopFake*>(p);
  f->restartCalls++;
  return f->restartTriggers;
}
crosspoint::mem::RecoveryActions actionsFor(LoopFake& f) {
  return crosspoint::mem::RecoveryActions{loopFakeAnchor, loopFakeMax, loopFakeRestart, &f};
}
}  // namespace

using crosspoint::mem::layoutHeapRecovered;
using crosspoint::mem::RecoveryActions;
using crosspoint::mem::RecoveryRun;
using crosspoint::mem::RecoverySeed;
using crosspoint::mem::runRecoveryLadder;

void test_loop_gate_already_open_runs_no_actions() {
  setLargestFreeBlockOverride(50 * 1024);  // above the 48K gate
  LoopFake f;
  RecoverySeed seed{true, true, 2};
  TEST_ASSERT_EQUAL(RecoveryRun::GateOpen, runRecoveryLadder(seed, actionsFor(f)));
  TEST_ASSERT_EQUAL_INT(0, f.anchorCalls);
  TEST_ASSERT_EQUAL_INT(0, f.maxCalls);
  TEST_ASSERT_EQUAL_INT(0, f.restartCalls);
}

void test_loop_anchor_release_alone_clears_gate() {
  setLargestFreeBlockOverride(30 * 1024);  // gate closed
  LoopFake f;
  f.afterAnchor = 50 * 1024;  // dropping the anchor crosses the 48K gate
  RecoverySeed seed{/*anchorHeld=*/true, /*bookOpen=*/true, /*budget=*/2};
  TEST_ASSERT_EQUAL(RecoveryRun::GateOpen, runRecoveryLadder(seed, actionsFor(f)));
  TEST_ASSERT_EQUAL_INT(1, f.anchorCalls);
  TEST_ASSERT_EQUAL_INT(0, f.maxCalls);  // never escalated past the anchor
  TEST_ASSERT_EQUAL_INT(0, f.restartCalls);
}

void test_loop_escalates_anchor_then_max_then_proceeds() {
  setLargestFreeBlockOverride(30 * 1024);
  LoopFake f;
  f.afterAnchor = 35 * 1024;  // still < 48K gate
  f.afterMax = 30 * 1024;     // >= 20K floor, < 48K gate -> Proceed optimistically
  RecoverySeed seed{/*anchorHeld=*/true, /*bookOpen=*/true, /*budget=*/2};
  TEST_ASSERT_EQUAL(RecoveryRun::GateOpen, runRecoveryLadder(seed, actionsFor(f)));
  TEST_ASSERT_EQUAL_INT(1, f.anchorCalls);
  TEST_ASSERT_EQUAL_INT(1, f.maxCalls);
  TEST_ASSERT_EQUAL_INT(0, f.restartCalls);
}

void test_loop_triggers_silent_restart_below_floor_with_budget() {
  setLargestFreeBlockOverride(10 * 1024);
  LoopFake f;
  f.afterAnchor = 10 * 1024;
  f.afterMax = 10 * 1024;  // below the 20K hard floor
  f.restartTriggers = true;
  RecoverySeed seed{/*anchorHeld=*/true, /*bookOpen=*/true, /*budget=*/2};
  TEST_ASSERT_EQUAL(RecoveryRun::Restarted, runRecoveryLadder(seed, actionsFor(f)));
  TEST_ASSERT_EQUAL_INT(1, f.anchorCalls);
  TEST_ASSERT_EQUAL_INT(1, f.maxCalls);
  TEST_ASSERT_EQUAL_INT(1, f.restartCalls);
}

void test_loop_gives_up_when_restart_reserve_lost() {
  setLargestFreeBlockOverride(10 * 1024);
  LoopFake f;
  f.afterAnchor = 10 * 1024;
  f.afterMax = 10 * 1024;
  f.restartTriggers = false;  // budget vanished between peek and claim
  RecoverySeed seed{/*anchorHeld=*/true, /*bookOpen=*/true, /*budget=*/2};
  TEST_ASSERT_EQUAL(RecoveryRun::GiveUp, runRecoveryLadder(seed, actionsFor(f)));
  TEST_ASSERT_EQUAL_INT(1, f.restartCalls);  // it tried
}

void test_loop_gives_up_below_floor_when_budget_zero_never_tries_restart() {
  setLargestFreeBlockOverride(10 * 1024);
  LoopFake f;
  f.afterMax = 10 * 1024;  // below floor
  RecoverySeed seed{/*anchorHeld=*/false, /*bookOpen=*/true, /*budget=*/0};
  TEST_ASSERT_EQUAL(RecoveryRun::GiveUp, runRecoveryLadder(seed, actionsFor(f)));
  TEST_ASSERT_EQUAL_INT(0, f.anchorCalls);  // anchor not held
  TEST_ASSERT_EQUAL_INT(1, f.maxCalls);
  TEST_ASSERT_EQUAL_INT(0, f.restartCalls);  // budget 0 -> GiveUp, restart never attempted
}

void test_loop_convenience_wrapper_maps_to_bool() {
  setLargestFreeBlockOverride(50 * 1024);
  LoopFake f1;
  TEST_ASSERT_TRUE(layoutHeapRecovered(RecoverySeed{true, true, 2}, actionsFor(f1)));

  setLargestFreeBlockOverride(10 * 1024);
  LoopFake f2;
  f2.afterMax = 10 * 1024;
  TEST_ASSERT_FALSE(layoutHeapRecovered(RecoverySeed{false, true, 0}, actionsFor(f2)));
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

// The global OOM new-handler: first call sheds once and stays armed (operator
// new will retry); a second consecutive call steps aside (clears the handler)
// so the allocation fails normally; installOomHandler re-arms and resets the
// per-episode shed budget.
void test_oom_handler_sheds_once_then_steps_aside_then_rearms() {
  static int sheds;
  sheds = 0;
  registerShedEvictor([]() { sheds++; });

  crosspoint::mem::installOomHandler();
  TEST_ASSERT_TRUE(std::get_new_handler() == &crosspoint::mem::oomNewHandler);

  crosspoint::mem::oomNewHandler();  // first failure: shed + stay armed
  TEST_ASSERT_EQUAL_INT(1, sheds);
  TEST_ASSERT_TRUE(std::get_new_handler() == &crosspoint::mem::oomNewHandler);

  crosspoint::mem::oomNewHandler();  // retry still failed: step aside
  TEST_ASSERT_EQUAL_INT(1, sheds);
  TEST_ASSERT_TRUE(std::get_new_handler() == nullptr);

  crosspoint::mem::installOomHandler();  // re-arm: fresh episode
  crosspoint::mem::oomNewHandler();
  TEST_ASSERT_EQUAL_INT(2, sheds);
  std::set_new_handler(nullptr);
}

// ── Shed-aware C-buffer allocation (tryMallocShed) ──────────────────────────
// malloc bypasses the C++ new-handler, so tryMalloc gets no shed-retry. The
// hook seam scripts allocation failure so the shed-retry branch is testable.

namespace {
int g_allocCalls;
bool g_shedRan;
// Returns null until a shed has run, then a real buffer — models the
// "fragmented + caches pinned, one shed frees enough" recovery case.
void* failUntilShed(size_t n) {
  g_allocCalls++;
  if (!g_shedRan) return nullptr;
  return std::malloc(n);
}
void* alwaysFail(size_t) {
  g_allocCalls++;
  return nullptr;
}
void* alwaysSucceed(size_t n) {
  g_allocCalls++;
  return std::malloc(n);
}
}  // namespace

void test_try_malloc_shed_succeeds_first_try_without_shedding() {
  g_allocCalls = 0;
  g_shedRan = false;
  registerShedEvictor([]() { g_shedRan = true; });
  crosspoint::mem::setTryMallocHookForTest(&alwaysSucceed);
  void* p = crosspoint::mem::tryMallocShed(1024);
  crosspoint::mem::clearTryMallocHookForTest();
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_INT(1, g_allocCalls);  // no retry needed
  TEST_ASSERT_FALSE(g_shedRan);            // ample → caches untouched
  std::free(p);
}

void test_try_malloc_shed_sheds_once_then_retries_and_succeeds() {
  g_allocCalls = 0;
  g_shedRan = false;
  registerShedEvictor([]() { g_shedRan = true; });
  crosspoint::mem::setTryMallocHookForTest(&failUntilShed);
  void* p = crosspoint::mem::tryMallocShed(1024);
  crosspoint::mem::clearTryMallocHookForTest();
  TEST_ASSERT_NOT_NULL(p);                 // recovered after shedding
  TEST_ASSERT_EQUAL_INT(2, g_allocCalls);  // failed, shed, retried
  TEST_ASSERT_TRUE(g_shedRan);
  std::free(p);
}

void test_try_malloc_shed_returns_null_when_shed_cannot_help() {
  g_allocCalls = 0;
  g_shedRan = false;
  registerShedEvictor([]() { g_shedRan = true; });
  crosspoint::mem::setTryMallocHookForTest(&alwaysFail);
  void* p = crosspoint::mem::tryMallocShed(1024);
  crosspoint::mem::clearTryMallocHookForTest();
  TEST_ASSERT_NULL(p);                     // genuinely exhausted
  TEST_ASSERT_EQUAL_INT(2, g_allocCalls);  // tried, shed, tried again
  TEST_ASSERT_TRUE(g_shedRan);             // shed was attempted
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
  RUN_TEST(test_loop_gate_already_open_runs_no_actions);
  RUN_TEST(test_loop_anchor_release_alone_clears_gate);
  RUN_TEST(test_loop_escalates_anchor_then_max_then_proceeds);
  RUN_TEST(test_loop_triggers_silent_restart_below_floor_with_budget);
  RUN_TEST(test_loop_gives_up_when_restart_reserve_lost);
  RUN_TEST(test_loop_gives_up_below_floor_when_budget_zero_never_tries_restart);
  RUN_TEST(test_loop_convenience_wrapper_maps_to_bool);
  RUN_TEST(test_room_to_grow_passes_without_shedding_when_heap_ample);
  RUN_TEST(test_room_to_grow_sheds_once_then_succeeds);
  RUN_TEST(test_room_to_grow_fails_when_shed_cannot_free_enough);
  RUN_TEST(test_shed_under_pressure_runs_all_registered_evictors);
  RUN_TEST(test_oom_handler_sheds_once_then_steps_aside_then_rearms);
  RUN_TEST(test_try_malloc_shed_succeeds_first_try_without_shedding);
  RUN_TEST(test_try_malloc_shed_sheds_once_then_retries_and_succeeds);
  RUN_TEST(test_try_malloc_shed_returns_null_when_shed_cannot_help);
  return UNITY_END();
}
