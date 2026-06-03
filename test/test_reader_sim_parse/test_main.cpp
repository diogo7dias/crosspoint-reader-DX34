// Host parse sim: run the REAL ChapterHtmlSlimParser (expat XHTML -> ParsedText
// layout -> Page) on a real EPUB chapter, under SimHeap. This is the "middle"
// of the open-a-book pipeline — the allocation-heaviest stage. Proves it is
// memory-stable under fragmentation and measures parse+layout timing.
//
// Pipeline: ZipFile inflates a real chapter -> temp file -> ChapterHtmlSlimParser
// parses it into Pages (delivered via callback).
//
// Run via: pio test -e test_sim_parse -f test_reader_sim_parse
#include <unity.h>

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HeapGuard.h>
#include <Page.h>
#include <Print.h>
#include <ZipFile.h>
#include <parsers/ChapterHtmlSlimParser.h>
#include <parsers/FootnotePlacer.h>
#include <parsers/StyleResolver.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "../test_sim_heap/SimHeap.h"

#ifndef SIM_EPUB_PATH
#define SIM_EPUB_PATH "test/test_reader_sim/fixtures/book.epub"
#endif

using crosspoint::test::SimHeap;

namespace {
const char* kChapterEntry = "OEBPS/7910875783089588439_1342-h-4.htm.html";
const char* kTmpHtml = "/tmp/crosspoint_sim_chapter.html";

class FilePrint : public Print {
 public:
  explicit FilePrint(FILE* f) : f_(f) {}
  size_t write(uint8_t b) override { return std::fputc(b, f_) == EOF ? 0 : 1; }
  size_t write(const uint8_t* p, size_t n) override { return std::fwrite(p, 1, n, f_); }

 private:
  FILE* f_;
};

// Inflate the chapter from the EPUB into a temp file (disarmed). Returns bytes.
size_t extractChapterToTmp() {
  FILE* tf = std::fopen(kTmpHtml, "wb");
  if (!tf) return 0;
  FilePrint fp(tf);
  std::string epubPath = SIM_EPUB_PATH;
  ZipFile zip(epubPath);
  zip.readFileToStream(kChapterEntry, fp, 4096);
  const long n = std::ftell(tf);
  std::fclose(tf);
  return n < 0 ? 0 : static_cast<size_t>(n);
}

GfxRenderer g_renderer;

// Parse the temp chapter file into pages. Returns page count.
uint32_t parseChapter() {
  auto epub = std::make_shared<Epub>(std::string(SIM_EPUB_PATH), std::string("/tmp"));
  std::string filepath = kTmpHtml;  // ctor stores this by reference — keep alive.
  uint32_t pages = 0;
  ChapterParseConfig cfg;
  cfg.fontId = 0;
  cfg.lineCompression = 1.0f;
  cfg.viewportWidth = 600;
  cfg.viewportHeight = 800;
  cfg.hyphenationEnabled = false;
  cfg.wordSpacingPercent = 1;
  cfg.usePublisherStyles = true;
  ChapterHtmlSlimParser parser(epub, filepath, g_renderer, cfg, [&pages](std::unique_ptr<Page>) { ++pages; },
                               [](const std::string&, uint16_t) {}, /*progressFn=*/nullptr, /*cssParser=*/nullptr);
  parser.parseAndBuildPages();
  return pages;
}
}  // namespace

void setUp() {
  SimHeap::reset();
  crosspoint::heap::clearLargestFreeBlockOverride();
}
void tearDown() {
  SimHeap::reset();
  crosspoint::heap::clearLargestFreeBlockOverride();
}

// The decompress->parse->layout chain runs on a real chapter and produces pages.
void test_parse_chapter_healthy() {
  const size_t bytes = extractChapterToTmp();
  TEST_ASSERT_GREATER_THAN_UINT(50000, bytes);  // ~72 KB chapter extracted

  SimHeap::arm(/*cap=*/8u * 1024 * 1024, /*budget=*/64u * 1024 * 1024);
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t pages = parseChapter();
  const auto t1 = std::chrono::steady_clock::now();
  const unsigned allocs = SimHeap::attempts();
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();

  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  char msg[160];
  snprintf(msg, sizeof(msg), "PARSE chapter: %u pages | %lld us | %u allocs | wouldAbort=%u", pages,
           static_cast<long long>(us), allocs, wouldAbort);
  TEST_MESSAGE(msg);

  // Fidelity gate (RFC #170): exact page count for this fixture. Style-resolution
  // drift would shift glyph widths -> line breaks -> page count. Update only on an
  // intentional layout change.
  TEST_ASSERT_EQUAL_UINT(57, pages);
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);
}

// Parse the real chapter under the v3.0.1 incident fragmentation. The parser
// must not reach a throwing allocation (it uses nothrow Page + probe-guarded
// ParsedText). Records wouldAbort: >0 would be a real unguarded-OOM finding.
void test_parse_chapter_under_fragmentation() {
  extractChapterToTmp();  // disarmed extraction

  crosspoint::heap::setLargestFreeBlockOverride(11764);
  SimHeap::arm(/*cap=*/11764, /*budget=*/142824);
  bool threw = false;
  uint32_t pages = 0;
  try {
    pages = parseChapter();
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();
  crosspoint::heap::clearLargestFreeBlockOverride();

  char msg[128];
  snprintf(msg, sizeof(msg), "PARSE@frag(11764): pages=%u threw=%d wouldAbort=%u", pages, threw ? 1 : 0, wouldAbort);
  TEST_MESSAGE(msg);
  TEST_ASSERT_FALSE(threw);
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);
}

// ---------------------------------------------------------------------------
// StyleResolver characterization tests (RFC #170). These pin the EXACT current
// three-system style-merge behaviour of ChapterHtmlSlimParser, including the
// known font-weight:normal-cannot-un-bold OR-quirk, so the extraction is
// behaviour-identical. No expat, no renderer, no Page.
// ---------------------------------------------------------------------------
namespace {
CssStyle cssItalic() {
  CssStyle c;
  c.fontStyle = CssFontStyle::Italic;
  c.defined.fontStyle = 1;
  return c;
}
CssStyle cssBold() {
  CssStyle c;
  c.fontWeight = CssFontWeight::Bold;
  c.defined.fontWeight = 1;
  return c;
}
}  // namespace

// Fresh resolver, plain text -> REGULAR.
void test_style_plain_is_regular() {
  StyleResolver r;
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::REGULAR, r.effectiveStyle(1));
}

// <b>: depth flag + inline stack entry -> BOLD for words inside it.
void test_style_bold_tag() {
  StyleResolver r;
  r.setBoldFrom(0);
  r.pushInline(0, {.hasBold = true, .bold = true});
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::BOLD, r.effectiveStyle(1));
}

// "<em>foo<b>bar</b></em>": foo ITALIC, bar BOLD_ITALIC, back to ITALIC after </b>.
void test_style_em_then_bold_nested() {
  StyleResolver r;
  r.pushInline(0, {.hasItalic = true, .italic = true});
  r.setItalicFrom(0);
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::ITALIC, r.effectiveStyle(1));  // "foo"

  r.pushInline(1, {.hasBold = true, .bold = true});
  r.setBoldFrom(1);
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::BOLD_ITALIC, r.effectiveStyle(2));  // "bar"

  r.clearDepthFlagsAt(1);
  r.popInlineAtDepth(1);
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::ITALIC, r.effectiveStyle(1));  // back in <em>
}

// Block-level CSS base (e.g. <p style="font-style:italic">) -> ITALIC, no depth flag.
void test_style_css_base_italic() {
  StyleResolver r;
  r.setCssBase(cssItalic());
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::ITALIC, r.effectiveStyle(1));
  r.clearCssBase();
  TEST_ASSERT_EQUAL_UINT8(EpdFontFamily::REGULAR, r.effectiveStyle(1));
}

// Underline (e.g. footnote <a>): UNDERLINE bit set.
void test_style_underline_via_anchor() {
  StyleResolver r;
  r.setUnderlineFrom(2);
  r.pushInline(2, {.hasUnderline = true, .underline = true});
  const auto s = r.effectiveStyle(3);
  TEST_ASSERT_TRUE(s & EpdFontFamily::UNDERLINE);
}

// FROZEN QUIRK (RFC #170): an inline font-weight:normal (stack bold=false) at a
// deeper depth CANNOT turn off bold while a shallower depth flag is active,
// because the merge is `(boldUntilDepth < depth) OR effectiveBold`. Pinned so
// the extraction reproduces it; the fix is a separate guarded follow-up.
void test_style_frozen_normal_cannot_unbold_under_depth_flag() {
  StyleResolver r;
  r.setBoldFrom(0);  // <b> at depth 0 sets boldUntilDepth = 0
  r.pushInline(2, {.hasBold = true, .bold = false});  // inner span font-weight:normal
  TEST_ASSERT_TRUE(r.effectiveStyle(3) & EpdFontFamily::BOLD);  // STILL bold (OR wins)
}

// Inline stack last-writer-wins when NO depth flag is active: a deeper
// font-weight:normal over a CSS-base bold does turn bold off.
void test_style_inline_normal_overrides_css_base_bold() {
  StyleResolver r;
  r.setCssBase(cssBold());                                       // base bold, no depth flag
  TEST_ASSERT_TRUE(r.effectiveStyle(1) & EpdFontFamily::BOLD);
  r.pushInline(1, {.hasBold = true, .bold = false});            // <span font-weight:normal>
  TEST_ASSERT_FALSE(r.effectiveStyle(2) & EpdFontFamily::BOLD);  // overridden off
}

// The 64-entry cap: pushes beyond MAX_INLINE_STYLE_DEPTH are dropped (false).
void test_style_inline_stack_cap() {
  StyleResolver r;
  for (size_t i = 0; i < StyleResolver::MAX_INLINE_STYLE_DEPTH; ++i) {
    TEST_ASSERT_TRUE(r.pushInline(static_cast<int>(i), {.hasItalic = true, .italic = true}));
  }
  TEST_ASSERT_FALSE(r.pushInline(1000, {.hasBold = true, .bold = true}));  // dropped
}

// wouldChangeAt(closingDepth): true iff a stack entry or depth flag sits there.
void test_style_would_change_at() {
  StyleResolver r;
  r.pushInline(2, {.hasBold = true, .bold = true});
  TEST_ASSERT_TRUE(r.wouldChangeAt(2));
  TEST_ASSERT_FALSE(r.wouldChangeAt(1));
  r.setItalicFrom(3);
  TEST_ASSERT_TRUE(r.wouldChangeAt(3));
}

// clearDepthFlagsAt clears only flags whose value == the given depth.
void test_style_clear_depth_flags_matches_depth_only() {
  StyleResolver r;
  r.setBoldFrom(2);
  r.clearDepthFlagsAt(3);  // 2 != 3 -> no clear
  TEST_ASSERT_TRUE(r.effectiveStyle(4) & EpdFontFamily::BOLD);
  r.clearDepthFlagsAt(2);  // 2 == 2 -> cleared
  TEST_ASSERT_FALSE(r.effectiveStyle(4) & EpdFontFamily::BOLD);
}

// ---------------------------------------------------------------------------
// FootnotePlacer characterization tests (RFC #170). Pin the word-index ->
// page assignment behaviour: register an anchor at a word index, advance per
// laid-out line, emit when the index is reached, fallback-drain at block end.
// ---------------------------------------------------------------------------
namespace {
FootnoteEntry fnEntry(const char* number, const char* href) {
  FootnoteEntry e;
  strncpy(e.number, number, sizeof(e.number) - 1);
  strncpy(e.href, href, sizeof(e.href) - 1);
  return e;
}
struct EmitLog {
  std::vector<std::string> numbers;
  FootnotePlacer::EmitFn fn() {
    return [this](const char* n, const char*) { numbers.emplace_back(n); };
  }
};
}  // namespace

void test_footnote_empty_initially() {
  FootnotePlacer p;
  TEST_ASSERT_TRUE(p.empty());
  TEST_ASSERT_EQUAL_INT(0, p.extractedWordCount());
}

// A footnote whose anchor word lands after a page break binds to the page that
// crosses its index, not an earlier one.
void test_footnote_placed_on_page_crossing_its_index() {
  FootnotePlacer p;
  p.registerFootnote(12, fnEntry("1", "ch.html#fn1"));
  EmitLog page0, page1;
  p.placeForLine(5, page0.fn());  // cumulative 5  -> nothing
  p.placeForLine(4, page0.fn());  // cumulative 9  -> nothing
  // page break here (caller moves to page1)
  p.placeForLine(4, page1.fn());  // cumulative 13 -> 12<=13 drains
  TEST_ASSERT_EQUAL_UINT(0, page0.numbers.size());
  TEST_ASSERT_EQUAL_UINT(1, page1.numbers.size());
  TEST_ASSERT_EQUAL_STRING("1", page1.numbers[0].c_str());
  TEST_ASSERT_TRUE(p.empty());
}

// Index exactly equal to the cumulative count drains on that line (<=).
void test_footnote_index_equal_to_count_drains() {
  FootnotePlacer p;
  p.registerFootnote(5, fnEntry("x", "h"));
  EmitLog log;
  p.placeForLine(5, log.fn());  // cumulative 5, 5<=5 -> drains
  TEST_ASSERT_EQUAL_UINT(1, log.numbers.size());
}

// Multiple footnotes at the same index drain together, in registration order.
void test_footnote_multiple_drain_in_order() {
  FootnotePlacer p;
  p.registerFootnote(3, fnEntry("A", "a"));
  p.registerFootnote(3, fnEntry("B", "b"));
  EmitLog log;
  p.placeForLine(3, log.fn());
  TEST_ASSERT_EQUAL_UINT(2, log.numbers.size());
  TEST_ASSERT_EQUAL_STRING("A", log.numbers[0].c_str());
  TEST_ASSERT_EQUAL_STRING("B", log.numbers[1].c_str());
}

// extractedWordCount tracks the cumulative laid-out words.
void test_footnote_extracted_count_advances() {
  FootnotePlacer p;
  EmitLog log;
  p.placeForLine(10, log.fn());
  p.placeForLine(7, log.fn());
  TEST_ASSERT_EQUAL_INT(17, p.extractedWordCount());
}

// drainRemaining flushes stragglers a line drain never reached.
void test_footnote_drain_remaining_fallback() {
  FootnotePlacer p;
  p.registerFootnote(1000, fnEntry("z", "h"));  // never reached by lines
  EmitLog log;
  p.placeForLine(3, log.fn());
  TEST_ASSERT_EQUAL_UINT(0, log.numbers.size());
  p.drainRemaining(log.fn());
  TEST_ASSERT_EQUAL_UINT(1, log.numbers.size());
  TEST_ASSERT_TRUE(p.empty());
}

// onNewBlock resets the cumulative counter but leaves pending footnotes.
void test_footnote_on_new_block_resets_count_keeps_pending() {
  FootnotePlacer p;
  EmitLog log;
  p.placeForLine(10, log.fn());
  p.registerFootnote(2, fnEntry("1", "h"));
  p.onNewBlock();
  TEST_ASSERT_EQUAL_INT(0, p.extractedWordCount());
  TEST_ASSERT_FALSE(p.empty());                 // pending preserved
  p.placeForLine(2, log.fn());                  // cumulative 2 -> 2<=2 drains
  TEST_ASSERT_EQUAL_UINT(1, log.numbers.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_chapter_healthy);
  RUN_TEST(test_parse_chapter_under_fragmentation);
  RUN_TEST(test_style_plain_is_regular);
  RUN_TEST(test_style_bold_tag);
  RUN_TEST(test_style_em_then_bold_nested);
  RUN_TEST(test_style_css_base_italic);
  RUN_TEST(test_style_underline_via_anchor);
  RUN_TEST(test_style_frozen_normal_cannot_unbold_under_depth_flag);
  RUN_TEST(test_style_inline_normal_overrides_css_base_bold);
  RUN_TEST(test_style_inline_stack_cap);
  RUN_TEST(test_style_would_change_at);
  RUN_TEST(test_style_clear_depth_flags_matches_depth_only);
  RUN_TEST(test_footnote_empty_initially);
  RUN_TEST(test_footnote_placed_on_page_crossing_its_index);
  RUN_TEST(test_footnote_index_equal_to_count_drains);
  RUN_TEST(test_footnote_multiple_drain_in_order);
  RUN_TEST(test_footnote_extracted_count_advances);
  RUN_TEST(test_footnote_drain_remaining_fallback);
  RUN_TEST(test_footnote_on_new_block_resets_count_keeps_pending);
  return UNITY_END();
}
