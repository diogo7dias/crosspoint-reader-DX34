/**
 * Host-side tests for reader::ReaderStatusComposer. Pure reserve/build logic
 * driven by a deterministic fake measurer; no ESP32 / SD / GfxRenderer. Runs:
 *   pio test -e test_host -f test_status_composer
 */
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "activities/reader/ReaderStatusComposer.h"

using crosspoint::reader::IStatusMeasurePort;
using crosspoint::reader::ReaderStatusComposer;
using crosspoint::reader::ReserveInput;
using crosspoint::reader::ReserveResult;
using crosspoint::reader::StatusBarSettings;
using crosspoint::reader::StatusTitleHooks;
using crosspoint::reader::StatusValues;
using Layout = ReaderStatusBar::StatusBarLayout;

namespace {

// Deterministic measurer: width = chars * 8 px; counts getTextWidth calls so a
// test can assert a title/sample was (or was not) re-measured.
class FakeMeasure : public IStatusMeasurePort {
 public:
  mutable int textWidthCalls = 0;
  int getScreenWidth() const override { return 480; }
  int getScreenHeight() const override { return 800; }
  int getLineHeight(int) const override { return 18; }
  int getTextHeight(int) const override { return 14; }
  int getTextWidth(int, const char* text) const override {
    ++textWidthCalls;
    return static_cast<int>(std::strlen(text)) * 8;
  }
  std::string truncatedText(int, const char* text, int maxWidth) const override {
    const int maxChars = maxWidth / 8;
    std::string s(text);
    if (static_cast<int>(s.size()) > maxChars && maxChars >= 0) s.resize(static_cast<size_t>(maxChars));
    return s;
  }
  void getOrientedViewableTRBL(int* t, int* r, int* b, int* l) const override { *t = *r = *b = *l = 0; }
};

StatusBarSettings enabledSettings() {
  StatusBarSettings s;
  s.enabled = true;
  s.fontId = 0;
  s.progressBarHeight = 2;
  return s;
}

StatusTitleHooks noTitleHooks() { return StatusTitleHooks{}; }

ReserveInput baseReserve() {
  ReserveInput in;
  in.screenHeight = 800;
  in.statusTopInset = 0;
  in.statusBottomInset = 0;
  in.marginTop = 0;
  in.marginBottom = 0;
  in.minContentHeight = 50;
  in.titleReserveWrapWidth = 472;
  return in;
}

// --- build() ---------------------------------------------------------------

void test_build_disabled_only_geometry() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s;  // enabled defaults to false
  s.showBookPercentage = true;
  StatusValues v;
  v.bookProgress = 50.0f;
  const Layout l = c.build(s, 400, ReserveResult{10, 20, 1}, v);
  TEST_ASSERT_EQUAL_INT(400, l.usableWidth);
  TEST_ASSERT_EQUAL_INT(10, l.topReservedHeight);
  TEST_ASSERT_EQUAL_INT(20, l.bottomReservedHeight);
  TEST_ASSERT_TRUE(l.bookPercentageText.empty());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, l.bookProgress);
}

void test_build_percentages_formatted() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showBookPercentage = true;
  s.showChapterPercentage = true;
  StatusValues v;
  v.bookProgress = 50.0f;
  v.chapterProgress = 30.0f;
  const Layout l = c.build(s, 400, ReserveResult{0, 0, 0}, v);
  TEST_ASSERT_EQUAL_STRING("B:50%", l.bookPercentageText.c_str());
  TEST_ASSERT_EQUAL_STRING("C:30%", l.chapterPercentageText.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(std::strlen("B:50%")) * 8, l.bookPercentageTextWidth);
}

void test_build_pages_left() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showPagesLeft = true;
  StatusValues v;
  v.pageCount = 10;
  v.currentPage0 = 2;  // remaining = 10 - 3 = 7
  v.pagesLeftLabel = "left";
  const Layout l = c.build(s, 400, ReserveResult{0, 0, 0}, v);
  TEST_ASSERT_EQUAL_STRING("7 left", l.pagesLeftText.c_str());
}

void test_build_pages_left_hidden_when_no_pages() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showPagesLeft = true;
  StatusValues v;
  v.pageCount = 0;
  v.pagesLeftLabel = "left";
  const Layout l = c.build(s, 400, ReserveResult{0, 0, 0}, v);
  TEST_ASSERT_TRUE(l.pagesLeftText.empty());
}

void test_build_free_heap() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showFreeHeap = true;
  StatusValues v;
  v.freeHeapBytes = 120u * 1024u;
  const Layout l = c.build(s, 400, ReserveResult{0, 0, 0}, v);
  TEST_ASSERT_EQUAL_STRING("RAM 120K", l.freeHeapText.c_str());
}

void test_build_reader_resolved_gated_on_nonempty() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showChapterNumber = true;
  s.showQuoteCount = true;
  s.showPageCounter = true;
  StatusValues v;
  // chapterNumber empty -> hidden even though shown; the rest provided.
  v.chapterNumberText = "";
  v.quoteCountText = "3 quotes";
  v.pageCounterText = "3/10";
  const Layout l = c.build(s, 400, ReserveResult{0, 0, 0}, v);
  TEST_ASSERT_TRUE(l.chapterNumberText.empty());
  TEST_ASSERT_EQUAL_STRING("3 quotes", l.quoteCountText.c_str());
  TEST_ASSERT_EQUAL_STRING("3/10", l.pageCounterText.c_str());
}

void test_build_title_cached() {
  FakeMeasure m;
  int titleTextCalls = 0;
  int titleKey = 5;
  StatusTitleHooks hooks;
  hooks.displayTitleKey = [&] { return titleKey; };
  hooks.displayTitleText = [&] {
    ++titleTextCalls;
    return std::string("Chapter Five");
  };
  ReaderStatusComposer c(m, "TST", std::move(hooks));
  StatusBarSettings s = enabledSettings();
  s.showChapterTitle = true;

  const Layout l1 = c.build(s, 400, ReserveResult{0, 0, 1}, StatusValues{});
  TEST_ASSERT_EQUAL_INT(1, titleTextCalls);
  TEST_ASSERT_FALSE(l1.titleLines.empty());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(l1.titleLines.size()), static_cast<int>(l1.titleLineWidths.size()));

  // Same key/width/maxLines -> cache hit, hook not called again.
  c.build(s, 400, ReserveResult{0, 0, 1}, StatusValues{});
  TEST_ASSERT_EQUAL_INT(1, titleTextCalls);

  // Key change -> re-resolved.
  titleKey = 6;
  c.build(s, 400, ReserveResult{0, 0, 1}, StatusValues{});
  TEST_ASSERT_EQUAL_INT(2, titleTextCalls);

  // invalidate -> re-resolved even on same key.
  c.invalidateTitleCaches();
  c.build(s, 400, ReserveResult{0, 0, 1}, StatusValues{});
  TEST_ASSERT_EQUAL_INT(3, titleTextCalls);
}

// --- reserve() -------------------------------------------------------------

void test_reserve_disabled_returns_default_title_count() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s;  // disabled
  s.showChapterTitle = true;
  const ReserveResult r = c.reserve(s, baseReserve());
  TEST_ASSERT_EQUAL_INT(0, r.topReservedHeight);
  TEST_ASSERT_EQUAL_INT(0, r.bottomReservedHeight);
  TEST_ASSERT_EQUAL_INT(1, r.resolvedTitleLineCount);
}

void test_reserve_top_item_reserves_top_only() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showPageCounter = true;
  s.pageCounterPosition = CrossPointSettings::STATUS_TEXT_TOP_LEFT;
  const ReserveResult r = c.reserve(s, baseReserve());
  TEST_ASSERT_TRUE(r.topReservedHeight > 0);
  TEST_ASSERT_EQUAL_INT(0, r.bottomReservedHeight);
}

void test_reserve_bottom_item_reserves_bottom_only() {
  FakeMeasure m;
  ReaderStatusComposer c(m, "TST", noTitleHooks());
  StatusBarSettings s = enabledSettings();
  s.showPageCounter = true;
  s.pageCounterPosition = CrossPointSettings::STATUS_TEXT_BOTTOM_RIGHT;
  const ReserveResult r = c.reserve(s, baseReserve());
  TEST_ASSERT_EQUAL_INT(0, r.topReservedHeight);
  TEST_ASSERT_TRUE(r.bottomReservedHeight > 0);
}

void test_reserve_title_samples_cached() {
  FakeMeasure m;
  int sampleCalls = 0;
  StatusTitleHooks hooks;
  hooks.reserveTitleKey = [] { return 1; };
  hooks.reserveTitleSamples = [&](int) {
    ++sampleCalls;
    return std::vector<std::string>{"A short title", "Another title here"};
  };
  ReaderStatusComposer c(m, "TST", std::move(hooks));
  StatusBarSettings s = enabledSettings();
  s.showChapterTitle = true;
  s.noTitleTruncation = true;
  s.titlePosition = CrossPointSettings::STATUS_AT_TOP;

  const ReserveResult r1 = c.reserve(s, baseReserve());
  TEST_ASSERT_EQUAL_INT(1, sampleCalls);
  TEST_ASSERT_TRUE(r1.resolvedTitleLineCount >= 1);

  // Same reserve key/width -> cache hit, samples not re-fetched.
  c.reserve(s, baseReserve());
  TEST_ASSERT_EQUAL_INT(1, sampleCalls);
}

}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_build_disabled_only_geometry);
  RUN_TEST(test_build_percentages_formatted);
  RUN_TEST(test_build_pages_left);
  RUN_TEST(test_build_pages_left_hidden_when_no_pages);
  RUN_TEST(test_build_free_heap);
  RUN_TEST(test_build_reader_resolved_gated_on_nonempty);
  RUN_TEST(test_build_title_cached);
  RUN_TEST(test_reserve_disabled_returns_default_title_count);
  RUN_TEST(test_reserve_top_item_reserves_top_only);
  RUN_TEST(test_reserve_bottom_item_reserves_bottom_only);
  RUN_TEST(test_reserve_title_samples_cached);
  return UNITY_END();
}
