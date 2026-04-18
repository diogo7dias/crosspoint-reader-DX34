/**
 * Host-side tests for reader::HighlightController. State machine + cursor
 * math only — no rendering, no Page, no SD. Runs via:
 *   pio test -e test_host -f test_highlight_controller
 */
#include <unity.h>

#include <climits>
#include <vector>

#include "activities/reader/HighlightController.h"

using crosspoint::reader::HighlightController;
using crosspoint::reader::MoveOutcome;
using crosspoint::reader::PageContext;
using crosspoint::reader::WordPos;

namespace {
std::vector<WordPos> makeWordsSingleLine(int count) {
  std::vector<WordPos> w;
  w.reserve(count);
  for (int i = 0; i < count; ++i) {
    w.push_back(WordPos{static_cast<int16_t>(10 + i * 40), 50, 30});
  }
  return w;
}

// Two lines of `perLine` words each, y=50 and y=90.
std::vector<WordPos> makeWordsTwoLines(int perLine) {
  std::vector<WordPos> w;
  w.reserve(perLine * 2);
  for (int line = 0; line < 2; ++line) {
    for (int i = 0; i < perLine; ++i) {
      w.push_back(WordPos{static_cast<int16_t>(10 + i * 40), static_cast<int16_t>(50 + line * 40), 30});
    }
  }
  return w;
}

PageContext ctx(int cur, int pc, int wc) { return PageContext{cur, pc, wc}; }
}  // namespace

void setUp() {}
void tearDown() {}

// ---- enter/exit invariants ----
void test_enter_sets_select_start() {
  HighlightController h;
  h.enter();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HighlightController::State::SELECT_START), static_cast<int>(h.state()));
  TEST_ASSERT_EQUAL_INT(0, h.cursorIndex());
  TEST_ASSERT_EQUAL_INT(-1, h.startPage());
  TEST_ASSERT_EQUAL_INT(0, h.wordCount());
}

void test_exit_clears_everything() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(3, makeWordsSingleLine(5));
  h.confirm(/*spine=*/2, /*page=*/3, /*nowMs=*/100);  // SELECT_START → SELECT_END

  h.exit();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HighlightController::State::NONE), static_cast<int>(h.state()));
  TEST_ASSERT_EQUAL_INT(0, h.cursorIndex());
  TEST_ASSERT_EQUAL_INT(-1, h.startPage());
  TEST_ASSERT_EQUAL_INT(-1, h.startWordIndex());
  TEST_ASSERT_EQUAL_INT(-1, h.endPage());
  TEST_ASSERT_EQUAL_INT(0, h.wordCount());
  TEST_ASSERT_EQUAL_INT(-1, h.cachedPage());
}

// ---- SELECT_START cursor clamping ----
void test_select_start_cursor_clamps() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(0, makeWordsSingleLine(4));

  auto r1 = h.moveCursor(+1, ctx(0, 1, 4));
  TEST_ASSERT_EQUAL_INT(0, r1.pageDelta);
  TEST_ASSERT_EQUAL_INT(1, h.cursorIndex());

  // Past the end just clamps, no page change.
  h.moveCursor(+1, ctx(0, 1, 4));
  h.moveCursor(+1, ctx(0, 1, 4));
  auto r4 = h.moveCursor(+1, ctx(0, 1, 4));
  TEST_ASSERT_EQUAL_INT(0, r4.pageDelta);
  TEST_ASSERT_EQUAL_INT(3, h.cursorIndex());
}

// ---- Confirm transitions ----
void test_confirm_select_start_captures_anchor() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(5, makeWordsSingleLine(4));
  h.moveCursor(+1, ctx(5, 10, 4));  // cursor=1
  h.moveCursor(+1, ctx(5, 10, 4));  // cursor=2

  auto r = h.confirm(/*spine=*/7, /*page=*/5, /*nowMs=*/100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HighlightController::State::SELECT_END), static_cast<int>(h.state()));
  TEST_ASSERT_EQUAL_INT(7, h.startSpine());
  TEST_ASSERT_EQUAL_INT(5, h.startPage());
  TEST_ASSERT_EQUAL_INT(2, h.startWordIndex());
  TEST_ASSERT_EQUAL_INT(5, h.endPage());
  TEST_ASSERT_EQUAL_INT(3, h.endWordIndex());  // last word on 4-word page
  TEST_ASSERT_TRUE(r.stateChanged);
  TEST_ASSERT_EQUAL_INT(0, r.pageDelta);
}

void test_confirm_select_end_jumps_back_and_starts_timer() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(3, makeWordsSingleLine(6));
  h.confirm(10, 3, 0);  // SELECT_END, endPage=3, endIdx=5
  h.setWordsForPage(4, makeWordsSingleLine(6));
  h.moveCursor(+1, ctx(3, 10, 6));  // pageDelta=+1 (off end), endPage=4, endIdx=0
  // simulate caller advancing to page 4
  auto r = h.confirm(10, 4, /*nowMs=*/2000);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(HighlightController::State::SHOW_UNDERLINE), static_cast<int>(h.state()));
  TEST_ASSERT_EQUAL_INT(-1, r.pageDelta);     // 3 (startPage) - 4 (current)
  TEST_ASSERT_EQUAL_INT(-1, h.cachedPage());  // invalidated
  TEST_ASSERT_EQUAL_INT(2000, static_cast<int>(h.underlineStartMs()));
}

// ---- Cross-page cursor (forward) ----
void test_end_cursor_crosses_page_forward() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(3, makeWordsSingleLine(4));
  h.confirm(0, 3, 0);  // SELECT_END, endPage=3, endIdx=3 (last word)

  // Page has 4 words, end cursor at 3. Another +1 must cross to page 4.
  auto r = h.moveCursor(+1, ctx(/*current=*/3, /*pages=*/10, /*words=*/4));
  TEST_ASSERT_EQUAL_INT(+1, r.pageDelta);
  TEST_ASSERT_TRUE(r.stateChanged);
  TEST_ASSERT_EQUAL_INT(4, h.endPage());
  TEST_ASSERT_EQUAL_INT(0, h.endWordIndex());
  TEST_ASSERT_EQUAL_INT(-1, h.cachedPage());
}

void test_end_cursor_last_page_no_cross() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(9, makeWordsSingleLine(3));
  h.confirm(0, 9, 0);  // endPage=9, endIdx=2

  // currentPage=9, pageCount=10 → lastPage. Can't cross.
  auto r = h.moveCursor(+1, ctx(9, 10, 3));
  TEST_ASSERT_EQUAL_INT(0, r.pageDelta);
  TEST_ASSERT_EQUAL_INT(2, h.endWordIndex());  // clamped to last word
}

// ---- Cross-page cursor (backward) ----
void test_end_cursor_crosses_page_backward() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(3, makeWordsSingleLine(5));
  h.confirm(0, 3, 0);  // SELECT_END, endPage=3, endIdx=4
  h.setWordsForPage(4, makeWordsSingleLine(5));
  h.moveCursor(+1, ctx(3, 10, 5));  // cross to page 4, endIdx=0
  // simulate caller advanced to page 4 and refreshed cache

  auto r = h.moveCursor(-1, ctx(4, 10, 5));
  TEST_ASSERT_EQUAL_INT(-1, r.pageDelta);
  TEST_ASSERT_EQUAL_INT(3, h.endPage());
  TEST_ASSERT_EQUAL_INT(INT_MAX, h.endWordIndex());  // sentinel for next-render clamp
  TEST_ASSERT_EQUAL_INT(-1, h.cachedPage());
}

void test_end_cursor_on_start_page_clamps_to_start_word() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(3, makeWordsSingleLine(6));
  h.moveCursor(+1, ctx(3, 10, 6));
  h.moveCursor(+1, ctx(3, 10, 6));  // cursor=2
  h.confirm(0, 3, 0);               // startPage=3, startWord=2, SELECT_END

  // End cursor was auto-set to word 5 (last on page). Drag it back.
  for (int i = 0; i < 5; ++i) h.moveCursor(-1, ctx(3, 10, 6));
  // Should clamp at startWordIndex=2 — can't go below start on start page.
  TEST_ASSERT_EQUAL_INT(2, h.endWordIndex());

  // Another -1 tries to go below 2. currentPage==startPage, so stays.
  auto r = h.moveCursor(-1, ctx(3, 10, 6));
  TEST_ASSERT_EQUAL_INT(0, r.pageDelta);
  TEST_ASSERT_EQUAL_INT(2, h.endWordIndex());
}

// ---- Word cache ----
void test_word_cache_set_and_invalidate() {
  HighlightController h;
  h.setWordsForPage(7, makeWordsSingleLine(3));
  TEST_ASSERT_EQUAL_INT(7, h.cachedPage());
  TEST_ASSERT_TRUE(h.wordCacheValidFor(7));
  TEST_ASSERT_FALSE(h.wordCacheValidFor(6));
  TEST_ASSERT_EQUAL_INT(3, h.wordCount());

  h.invalidateWordCache();
  TEST_ASSERT_FALSE(h.wordCacheValidFor(7));
  // words_ may still be populated; caller's job to refresh.
}

void test_on_page_changed_invalidates_when_different() {
  HighlightController h;
  h.setWordsForPage(5, makeWordsSingleLine(3));
  h.onPageChanged(5);
  TEST_ASSERT_TRUE(h.wordCacheValidFor(5));
  h.onPageChanged(6);
  TEST_ASSERT_FALSE(h.wordCacheValidFor(6));
}

// ---- Underline timeout ----
void test_underline_timeout() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(2, makeWordsSingleLine(3));
  h.confirm(0, 2, 0);     // SELECT_END
  h.confirm(0, 2, 1000);  // SHOW_UNDERLINE @ 1000

  TEST_ASSERT_FALSE(h.underlineTimedOut(1000));
  TEST_ASSERT_FALSE(h.underlineTimedOut(2000));
  TEST_ASSERT_FALSE(h.underlineTimedOut(3999));
  TEST_ASSERT_TRUE(h.underlineTimedOut(4000));  // 1000 + 3000
  TEST_ASSERT_TRUE(h.underlineTimedOut(9999));
}

void test_underline_timeout_false_when_not_showing() {
  HighlightController h;
  h.enter();
  TEST_ASSERT_FALSE(h.underlineTimedOut(UINT32_MAX));
  h.setWordsForPage(0, makeWordsSingleLine(3));
  h.confirm(0, 0, 0);  // SELECT_END
  TEST_ASSERT_FALSE(h.underlineTimedOut(UINT32_MAX));
}

// ---- Line-based cursor ----
void test_line_move_nearest_x_on_target_line() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(0, makeWordsTwoLines(4));  // y=50 (0..3), y=90 (4..7)
  // cursor at word 1 (x=50, y=50)
  h.moveCursor(+1, ctx(0, 1, 8));
  TEST_ASSERT_EQUAL_INT(1, h.cursorIndex());

  auto r = h.moveCursorLine(+1, ctx(0, 1, 8));
  TEST_ASSERT_TRUE(r.stateChanged);
  // Target line has words at x=10,50,90,130. Closest to x=50 is word 5 (x=50).
  TEST_ASSERT_EQUAL_INT(5, h.cursorIndex());
}

void test_line_move_no_line_falls_through_to_word() {
  HighlightController h;
  h.enter();
  h.setWordsForPage(0, makeWordsSingleLine(4));
  // Single line only — moving up/down has no target; falls through to moveCursor.
  h.moveCursor(+1, ctx(0, 1, 4));  // cursor=1
  auto r = h.moveCursorLine(+1, ctx(0, 1, 4));
  // Fallthrough advances by 1 in SELECT_START within-page.
  TEST_ASSERT_EQUAL_INT(2, h.cursorIndex());
  TEST_ASSERT_TRUE(r.stateChanged);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_enter_sets_select_start);
  RUN_TEST(test_exit_clears_everything);
  RUN_TEST(test_select_start_cursor_clamps);
  RUN_TEST(test_confirm_select_start_captures_anchor);
  RUN_TEST(test_confirm_select_end_jumps_back_and_starts_timer);
  RUN_TEST(test_end_cursor_crosses_page_forward);
  RUN_TEST(test_end_cursor_last_page_no_cross);
  RUN_TEST(test_end_cursor_crosses_page_backward);
  RUN_TEST(test_end_cursor_on_start_page_clamps_to_start_word);
  RUN_TEST(test_word_cache_set_and_invalidate);
  RUN_TEST(test_on_page_changed_invalidates_when_different);
  RUN_TEST(test_underline_timeout);
  RUN_TEST(test_underline_timeout_false_when_not_showing);
  RUN_TEST(test_line_move_nearest_x_on_target_line);
  RUN_TEST(test_line_move_no_line_falls_through_to_word);
  return UNITY_END();
}
