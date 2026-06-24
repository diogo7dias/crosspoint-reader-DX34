// Host tests for the pure reservoir-sampling move picker.
// Run via: pio test -e test_host -f test_sleep_move_selection
//
// reservoirSampleNames picks up to N names uniformly at random from a streaming
// source using O(N) memory — it never materializes the whole folder. That keeps
// the "move X random wallpapers to /sleep pause" action memory-safe even on a
// folder of 1000+ images. These tests drive it with a deterministic randomFn so
// the selection is exactly predictable.

#include <unity.h>

#include <string>
#include <vector>

#include "sleep/SleepMoveSelection.h"

using crosspoint::sleep::reservoirSampleNames;

namespace {

// A streaming source over a fixed vector of names.
crosspoint::sleep::NameSource makeSource(const std::vector<std::string>& names, size_t& cursor) {
  cursor = 0;
  return [&names, &cursor](std::string& out) -> bool {
    if (cursor >= names.size()) return false;
    out = names[cursor++];
    return true;
  };
}

}  // namespace

// Asking for more than exist returns every name (no random calls needed).
void test_count_exceeds_total_returns_all() {
  std::vector<std::string> names = {"a", "b", "c"};
  size_t cur = 0;
  int randomCalls = 0;
  auto rnd = [&randomCalls](long) -> long {
    ++randomCalls;
    return 0;
  };
  auto out = reservoirSampleNames(makeSource(names, cur), 5, rnd);

  TEST_ASSERT_EQUAL_UINT(3, out.size());
  TEST_ASSERT_EQUAL_STRING("a", out[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b", out[1].c_str());
  TEST_ASSERT_EQUAL_STRING("c", out[2].c_str());
  TEST_ASSERT_EQUAL_INT(0, randomCalls);  // fill-only, no eviction rolls
}

// Asking for zero returns nothing.
void test_zero_count() {
  std::vector<std::string> names = {"a", "b", "c"};
  size_t cur = 0;
  auto rnd = [](long) -> long { return 0; };
  auto out = reservoirSampleNames(makeSource(names, cur), 0, rnd);
  TEST_ASSERT_EQUAL_UINT(0, out.size());
}

// randomFn that never lands inside the reservoir keeps the first `count` items.
// For item index i (>= count) j = randomFn(i+1); returning i means j == i >= count,
// so no eviction ever happens.
void test_no_eviction_keeps_first_count() {
  std::vector<std::string> names = {"a", "b", "c", "d", "e"};
  size_t cur = 0;
  long seen = 0;
  auto rnd = [&seen](long n) -> long {
    seen = n;
    return n - 1;  // == i, never < count
  };
  auto out = reservoirSampleNames(makeSource(names, cur), 2, rnd);

  TEST_ASSERT_EQUAL_UINT(2, out.size());
  TEST_ASSERT_EQUAL_STRING("a", out[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b", out[1].c_str());
}

// randomFn that always returns 0 evicts slot 0 on every later item, so slot 0
// ends up holding the LAST streamed item and slot 1 keeps "b".
// Trace (count=2): fill [a,b]; i=2 j=0 -> [c,b]; i=3 j=0 -> [d,b]; i=4 j=0 -> [e,b].
void test_always_evict_slot_zero() {
  std::vector<std::string> names = {"a", "b", "c", "d", "e"};
  size_t cur = 0;
  auto rnd = [](long) -> long { return 0; };
  auto out = reservoirSampleNames(makeSource(names, cur), 2, rnd);

  TEST_ASSERT_EQUAL_UINT(2, out.size());
  TEST_ASSERT_EQUAL_STRING("e", out[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b", out[1].c_str());
}

// The sample is always a subset of distinct inputs (no duplicates introduced).
void test_sample_is_distinct_subset() {
  std::vector<std::string> names = {"0", "1", "2", "3", "4", "5", "6", "7"};
  size_t cur = 0;
  // Deterministic pseudo-spread: rotate through values.
  long tick = 0;
  auto rnd = [&tick](long n) -> long { return (tick++) % n; };
  auto out = reservoirSampleNames(makeSource(names, cur), 4, rnd);

  TEST_ASSERT_EQUAL_UINT(4, out.size());
  // All distinct and present in the input.
  for (size_t i = 0; i < out.size(); ++i) {
    for (size_t j = i + 1; j < out.size(); ++j) {
      TEST_ASSERT_FALSE(out[i] == out[j]);
    }
    bool found = false;
    for (const auto& n : names)
      if (n == out[i]) found = true;
    TEST_ASSERT_TRUE(found);
  }
}

// Empty source yields an empty sample.
void test_empty_source() {
  std::vector<std::string> names = {};
  size_t cur = 0;
  auto rnd = [](long) -> long { return 0; };
  auto out = reservoirSampleNames(makeSource(names, cur), 3, rnd);
  TEST_ASSERT_EQUAL_UINT(0, out.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_count_exceeds_total_returns_all);
  RUN_TEST(test_zero_count);
  RUN_TEST(test_no_eviction_keeps_first_count);
  RUN_TEST(test_always_evict_slot_zero);
  RUN_TEST(test_sample_is_distinct_subset);
  RUN_TEST(test_empty_source);
  return UNITY_END();
}
