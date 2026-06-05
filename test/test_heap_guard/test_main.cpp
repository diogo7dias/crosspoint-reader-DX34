// Host-side tests for the shared HeapGuard probe utility.
//
// Run via: pio test -e test_host -f test_heap_guard
#include <unity.h>

#include <cstdint>

#include "HeapGuard.h"

using crosspoint::heap::canAllocateContiguous;
using crosspoint::heap::clearLargestFreeBlockOverride;
using crosspoint::heap::kDefaultHeadroomBytes;
using crosspoint::heap::largestFreeBlockBytes;
using crosspoint::heap::setLargestFreeBlockOverride;

void setUp() { clearLargestFreeBlockOverride(); }
void tearDown() { clearLargestFreeBlockOverride(); }

void test_unset_override_returns_size_max() { TEST_ASSERT_EQUAL_size_t(SIZE_MAX, largestFreeBlockBytes()); }

void test_unset_override_allows_any_allocation() {
  TEST_ASSERT_TRUE(canAllocateContiguous(0));
  TEST_ASSERT_TRUE(canAllocateContiguous(64 * 1024));
  TEST_ASSERT_TRUE(canAllocateContiguous(1024 * 1024));
}

void test_override_blocks_oversize_alloc() {
  setLargestFreeBlockOverride(10 * 1024);
  TEST_ASSERT_FALSE(canAllocateContiguous(20 * 1024));
}

void test_override_permits_alloc_with_headroom() {
  // Largest free block 50 KB, need 30 KB, headroom 4 KB -> 34 KB <= 50 KB -> true.
  setLargestFreeBlockOverride(50 * 1024);
  TEST_ASSERT_TRUE(canAllocateContiguous(30 * 1024));
}

void test_override_refuses_when_headroom_pushes_over() {
  // Largest 10 KB, need 8 KB, headroom 4 KB -> 12 KB > 10 KB -> false.
  setLargestFreeBlockOverride(10 * 1024);
  TEST_ASSERT_FALSE(canAllocateContiguous(8 * 1024));
}

void test_custom_headroom_overrides_default() {
  setLargestFreeBlockOverride(10 * 1024);
  // need 8 KB + custom 1 KB = 9 KB <= 10 KB -> true (default would have failed).
  TEST_ASSERT_TRUE(canAllocateContiguous(8 * 1024, 1024));
  // need 8 KB + custom 4 KB = 12 KB > 10 KB -> false.
  TEST_ASSERT_FALSE(canAllocateContiguous(8 * 1024, 4 * 1024));
}

void test_overflow_request_refused() {
  // SIZE_MAX needBytes + any headroom wraps; must be refused.
  setLargestFreeBlockOverride(SIZE_MAX);
  TEST_ASSERT_FALSE(canAllocateContiguous(SIZE_MAX, 1));
  TEST_ASSERT_FALSE(canAllocateContiguous(SIZE_MAX - 100, 1024));
}

void test_zero_bytes_with_zero_headroom_always_ok_under_any_heap() {
  setLargestFreeBlockOverride(0);
  // need=0 + headroom=0 = 0; 0 <= 0 -> true. Degenerate but defined.
  TEST_ASSERT_TRUE(canAllocateContiguous(0, 0));
}

void test_zero_bytes_with_default_headroom_refused_when_heap_tiny() {
  setLargestFreeBlockOverride(1024);
  // need=0 but default headroom=4 KB; refused.
  TEST_ASSERT_FALSE(canAllocateContiguous(0));
}

void test_exactly_matching_request_passes() {
  setLargestFreeBlockOverride(10000);
  // need=6000 + default headroom=4096 = 10096 > 10000 -> false.
  TEST_ASSERT_FALSE(canAllocateContiguous(6000));
  // need=5904 + 4096 = 10000 == 10000 -> true.
  TEST_ASSERT_TRUE(canAllocateContiguous(5904));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unset_override_returns_size_max);
  RUN_TEST(test_unset_override_allows_any_allocation);
  RUN_TEST(test_override_blocks_oversize_alloc);
  RUN_TEST(test_override_permits_alloc_with_headroom);
  RUN_TEST(test_override_refuses_when_headroom_pushes_over);
  RUN_TEST(test_custom_headroom_overrides_default);
  RUN_TEST(test_overflow_request_refused);
  RUN_TEST(test_zero_bytes_with_zero_headroom_always_ok_under_any_heap);
  RUN_TEST(test_zero_bytes_with_default_headroom_refused_when_heap_tiny);
  RUN_TEST(test_exactly_matching_request_passes);
  return UNITY_END();
}
