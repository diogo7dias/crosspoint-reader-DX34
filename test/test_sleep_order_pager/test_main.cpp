// Host-side tests for the pure sleep-wallpaper order pager.
// Run via: pio test -e test_host -f test_sleep_order_pager
//
// The pager is the memory-safe core behind the "View sleep wallpapers"
// settings screen: it walks the rotation-order source one line at a time and
// materializes only the names on the requested page, never the whole list.
// These tests drive it through a fake line reader backed by a vector so the
// logic is exercised without any SD card.

#include <unity.h>

#include <string>
#include <vector>

#include "sleep/SleepOrderPager.h"

using crosspoint::sleep::OrderPage;
using crosspoint::sleep::readSleepOrderPage;

namespace {

// Build a line reader that hands out `lines` one at a time, mirroring an SD
// file streamed with readStringUntil('\n').
crosspoint::sleep::OrderLineReader makeReader(const std::vector<std::string>& lines, size_t& cursor) {
  cursor = 0;
  return [&lines, &cursor](std::string& out) -> bool {
    if (cursor >= lines.size()) return false;
    out = lines[cursor++];
    return true;
  };
}

}  // namespace

// A real order file starts with the "v1 cursor=N" header; it must be skipped
// and not counted as a wallpaper entry.
void test_header_is_skipped_and_not_counted() {
  std::vector<std::string> lines = {"v1 cursor=3", "a.bmp", "b.pxc", "c.bmp"};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 0, 10);

  TEST_ASSERT_EQUAL_UINT(3, page.total);
  TEST_ASSERT_EQUAL_UINT(3, page.names.size());
  TEST_ASSERT_EQUAL_STRING("a.bmp", page.names[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b.pxc", page.names[1].c_str());
  TEST_ASSERT_EQUAL_STRING("c.bmp", page.names[2].c_str());
}

// Pagination: only the [start, start+count) window is materialized, while
// total still reflects the whole list (for "row X / N" + clamping).
void test_pagination_window() {
  std::vector<std::string> lines = {"v1 cursor=0", "0.bmp", "1.bmp", "2.bmp", "3.bmp", "4.bmp"};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 2, 2);

  TEST_ASSERT_EQUAL_UINT(5, page.total);
  TEST_ASSERT_EQUAL_UINT(2, page.names.size());
  TEST_ASSERT_EQUAL_STRING("2.bmp", page.names[0].c_str());
  TEST_ASSERT_EQUAL_STRING("3.bmp", page.names[1].c_str());
}

// A source with no header (legacy/fallback) treats every line as an entry.
void test_no_header_all_entries() {
  std::vector<std::string> lines = {"x.bmp", "y.bmp"};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 0, 10);

  TEST_ASSERT_EQUAL_UINT(2, page.total);
  TEST_ASSERT_EQUAL_UINT(2, page.names.size());
  TEST_ASSERT_EQUAL_STRING("x.bmp", page.names[0].c_str());
}

// Blank/trailing lines are tolerated and not counted as entries.
void test_blank_lines_ignored() {
  std::vector<std::string> lines = {"v1 cursor=0", "a.bmp", "", "b.bmp", ""};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 0, 10);

  TEST_ASSERT_EQUAL_UINT(2, page.total);
  TEST_ASSERT_EQUAL_UINT(2, page.names.size());
  TEST_ASSERT_EQUAL_STRING("a.bmp", page.names[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b.bmp", page.names[1].c_str());
}

// A page starting past the end yields no names but still reports the true total.
void test_start_past_end() {
  std::vector<std::string> lines = {"v1 cursor=0", "a.bmp", "b.bmp"};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 9, 5);

  TEST_ASSERT_EQUAL_UINT(2, page.total);
  TEST_ASSERT_EQUAL_UINT(0, page.names.size());
}

// count == 0 collects nothing but still scans for the total.
void test_zero_count() {
  std::vector<std::string> lines = {"v1 cursor=0", "a.bmp", "b.bmp", "c.bmp"};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 0, 0);

  TEST_ASSERT_EQUAL_UINT(3, page.total);
  TEST_ASSERT_EQUAL_UINT(0, page.names.size());
}

// Empty source: no header, no entries.
void test_empty_source() {
  std::vector<std::string> lines = {};
  size_t cursor = 0;
  OrderPage page = readSleepOrderPage(makeReader(lines, cursor), 0, 10);

  TEST_ASSERT_EQUAL_UINT(0, page.total);
  TEST_ASSERT_EQUAL_UINT(0, page.names.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_header_is_skipped_and_not_counted);
  RUN_TEST(test_pagination_window);
  RUN_TEST(test_no_header_all_entries);
  RUN_TEST(test_blank_lines_ignored);
  RUN_TEST(test_start_past_end);
  RUN_TEST(test_zero_count);
  RUN_TEST(test_empty_source);
  return UNITY_END();
}
