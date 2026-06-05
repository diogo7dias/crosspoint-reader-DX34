// SimHeap unit tests — prove the allocation-failure injector models an
// ESP32-C3 fragmented heap and distinguishes throwing (would-abort) from
// nothrow (survives) allocation failures.
//
// Run via: pio test -e test_host -f test_sim_heap
//
// NOTE: while SimHeap is armed, EVERY allocation in this TU is modelled —
// including incidental ones in the test harness. So each test keeps the armed
// window as tight as possible (arm -> the allocation under test -> read
// counters into locals -> disarm) and avoids std::string / container growth
// inside the window. Use ::operator new(n) directly so the compiler can't
// elide a new-expression.
#include <unity.h>

#include <new>

#include "SimHeap.h"

using crosspoint::test::SimHeap;

void setUp() { SimHeap::reset(); }
void tearDown() { SimHeap::reset(); }

namespace {

// Disarmed (default): operator new/delete pass through, nothing modelled.
void test_disarmed_passthrough_no_modelling() {
  void* p = ::operator new(64 * 1024);  // huge, but disarmed -> real malloc
  TEST_ASSERT_NOT_NULL(p);
  ::operator delete(p);
  TEST_ASSERT_FALSE(SimHeap::armed());
  TEST_ASSERT_EQUAL_UINT(0, SimHeap::attempts());
}

// Armed, request <= cap and within budget: succeeds, liveBytes tracked, and
// delete returns it.
void test_armed_alloc_within_cap_succeeds_and_tracks() {
  SimHeap::arm(/*cap=*/64 * 1024, /*budget=*/200 * 1024);
  void* p = ::operator new(20 * 1024);
  const size_t live = SimHeap::liveBytes();
  const unsigned waDuring = SimHeap::wouldAbortThrows();
  ::operator delete(p);
  const size_t after = SimHeap::liveBytes();
  SimHeap::disarm();

  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_size_t(20 * 1024, live);
  TEST_ASSERT_EQUAL_size_t(0, after);
  TEST_ASSERT_EQUAL_UINT(0, waDuring);
}

// Armed, nothrow request > cap: returns nullptr (the SAFE path the hardening
// sweep installs). No throw, counted as a survived OOM.
void test_armed_nothrow_over_cap_returns_null() {
  SimHeap::arm(/*cap=*/10 * 1024, /*budget=*/200 * 1024);
  void* p = ::operator new(20 * 1024, std::nothrow);
  const unsigned nn = SimHeap::nothrowNulls();
  const unsigned wa = SimHeap::wouldAbortThrows();
  const unsigned ff = SimHeap::fragFails();
  SimHeap::disarm();

  TEST_ASSERT_NULL(p);
  TEST_ASSERT_EQUAL_UINT(1, nn);
  TEST_ASSERT_EQUAL_UINT(1, ff);
  TEST_ASSERT_EQUAL_UINT(0, wa);  // nothrow never "aborts"
}

// Armed, THROWING request > cap: throws std::bad_alloc. On-device (-fno-
// exceptions) this is the abort -> RTC_SW_SYS_RST. The sim records it as a
// would-abort so a stability test can fail on it.
void test_armed_throwing_over_cap_would_abort() {
  SimHeap::arm(/*cap=*/10 * 1024, /*budget=*/200 * 1024);
  bool threw = false;
  try {
    volatile void* p = ::operator new(20 * 1024);  // throwing form
    (void)p;
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wa = SimHeap::wouldAbortThrows();
  SimHeap::disarm();

  TEST_ASSERT_TRUE(threw);
  TEST_ASSERT_EQUAL_UINT(1, wa);
}

// Budget exhaustion: each request fits under cap, but cumulative live bytes
// exceed the total heap budget.
void test_armed_budget_exhaustion() {
  SimHeap::arm(/*cap=*/64 * 1024, /*budget=*/50 * 1024);
  void* a = ::operator new(30 * 1024, std::nothrow);  // ok: live=30k
  void* b = ::operator new(30 * 1024, std::nothrow);  // 60k > 50k budget -> null
  const unsigned ef = SimHeap::exhaustFails();
  SimHeap::disarm();

  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_NULL(b);
  TEST_ASSERT_EQUAL_UINT(1, ef);
  ::operator delete(a);
}

// THE INCIDENT: the v3.0.1 user's heap_report.txt — largest contiguous block
// 11764 bytes, total heap 142824. The font hot-group decompress wants ~20 KB.
// Throwing new -> device abort (the crash). Nothrow new -> nullptr, the path
// FontDecompressor takes (and the render-OOM guard then discards the frame).
void test_reproduces_v301_render_oom_incident() {
  constexpr size_t kUserLargestBlock = 11764;     // heap_report.txt "Largest 8-bit"
  constexpr size_t kUserTotalHeap = 142824;       // heap_report.txt "Total heap"
  constexpr size_t kHotGroupRequest = 20 * 1024;  // representative glyph group

  // Throwing path = what an unguarded `new`/vector::resize does on-device.
  SimHeap::arm(kUserLargestBlock, kUserTotalHeap);
  bool threw = false;
  try {
    volatile void* p = ::operator new(kHotGroupRequest);
    (void)p;
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wa = SimHeap::wouldAbortThrows();
  SimHeap::disarm();

  // Nothrow path = the FontDecompressor malloc / hardening sweep behaviour.
  SimHeap::arm(kUserLargestBlock, kUserTotalHeap);
  void* survived = ::operator new(kHotGroupRequest, std::nothrow);
  const unsigned nn = SimHeap::nothrowNulls();
  SimHeap::disarm();

  TEST_ASSERT_TRUE(threw);  // unguarded alloc WOULD crash the firmware
  TEST_ASSERT_EQUAL_UINT(1, wa);
  TEST_ASSERT_NULL(survived);  // guarded alloc fails cleanly instead
  TEST_ASSERT_EQUAL_UINT(1, nn);
}

// Healthy heap: a burst of small allocations never trips a would-abort.
void test_healthy_heap_no_would_abort() {
  SimHeap::arm(/*cap=*/90 * 1024, /*budget=*/200 * 1024);
  void* ptrs[32];
  for (int i = 0; i < 32; ++i) ptrs[i] = ::operator new(512, std::nothrow);
  const unsigned wa = SimHeap::wouldAbortThrows();
  const unsigned nn = SimHeap::nothrowNulls();
  for (int i = 0; i < 32; ++i) ::operator delete(ptrs[i]);
  const size_t after = SimHeap::liveBytes();
  SimHeap::disarm();

  TEST_ASSERT_EQUAL_UINT(0, wa);
  TEST_ASSERT_EQUAL_UINT(0, nn);
  TEST_ASSERT_EQUAL_size_t(0, after);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_disarmed_passthrough_no_modelling);
  RUN_TEST(test_armed_alloc_within_cap_succeeds_and_tracks);
  RUN_TEST(test_armed_nothrow_over_cap_returns_null);
  RUN_TEST(test_armed_throwing_over_cap_would_abort);
  RUN_TEST(test_armed_budget_exhaustion);
  RUN_TEST(test_reproduces_v301_render_oom_incident);
  RUN_TEST(test_healthy_heap_no_would_abort);
  return UNITY_END();
}
