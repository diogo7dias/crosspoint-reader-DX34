// Host parse sim: run the REAL ChapterHtmlSlimParser (expat XHTML -> ParsedText
// layout -> Page) on a real EPUB chapter, under SimHeap. This is the "middle"
// of the open-a-book pipeline — the allocation-heaviest stage. Proves it is
// memory-stable under fragmentation and measures parse+layout timing.
//
// Pipeline: ZipFile inflates a real chapter -> temp file -> ChapterHtmlSlimParser
// parses it into Pages (delivered via callback).
//
// Run via: pio test -e test_sim_parse -f test_reader_sim_parse
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HeapGuard.h>
#include <Page.h>
#include <Print.h>
#include <ZipFile.h>
#include <page/PageBuilder.h>
#include <parsers/ChapterHtmlSlimParser.h>
#include <parsers/FootnotePlacer.h>
#include <parsers/StyleResolver.h>
#include <unity.h>

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
  ChapterHtmlSlimParser parser(
      epub, filepath, g_renderer, cfg, [&pages](std::unique_ptr<Page>) { ++pages; },
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

// RFC #170 step 4: an inline font-weight:normal (stack bold=false) at a deeper
// depth DOES turn off bold set by a shallower ancestor depth flag (<b>/header).
// The depth flag is the outermost contribution; a deeper explicit inline
// setting overrides it (CSS cascade). This fixes the former OR-merge quirk.
void test_style_inline_normal_unbolds_under_depth_flag() {
  StyleResolver r;
  r.setBoldFrom(0);                                   // ancestor <b>/header at depth 0 sets boldUntilDepth = 0
  r.pushInline(2, {.hasBold = true, .bold = false});  // inner span font-weight:normal
  TEST_ASSERT_FALSE(r.effectiveStyle(3) & EpdFontFamily::BOLD);  // explicit normal wins
}

// Inline stack last-writer-wins when NO depth flag is active: a deeper
// font-weight:normal over a CSS-base bold does turn bold off.
void test_style_inline_normal_overrides_css_base_bold() {
  StyleResolver r;
  r.setCssBase(cssBold());  // base bold, no depth flag
  TEST_ASSERT_TRUE(r.effectiveStyle(1) & EpdFontFamily::BOLD);
  r.pushInline(1, {.hasBold = true, .bold = false});             // <span font-weight:normal>
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
  TEST_ASSERT_FALSE(p.empty());  // pending preserved
  p.placeForLine(2, log.fn());   // cumulative 2 -> 2<=2 drains
  TEST_ASSERT_EQUAL_UINT(1, log.numbers.size());
}

// — RFC #164 step 3: dp[]/ans[] line-break scratch backed by the LayoutArena —

// Lay out `nWords` identical words in one paragraph; return the per-line word
// counts. `arena` (nullable) backs the DP scratch when present.
static std::vector<size_t> layoutLineWordCounts(int nWords, crosspoint::layout::LayoutArena* arena) {
  ParsedText pt(/*extraSpacing=*/false, /*hyphenation=*/false, BlockStyle(), /*wordSpacing=*/1,
                /*firstLineIndentMode=*/0, /*usePublisherStyles=*/true, arena);
  for (int i = 0; i < nWords; ++i) pt.addWord("word", EpdFontFamily::REGULAR, false, false);
  std::vector<size_t> counts;
  pt.layoutAndExtractLines(
      g_renderer, 0, 600,
      [&counts](std::shared_ptr<TextBlock> tb) {
        if (tb) counts.push_back(tb->wordCount());
      },
      /*includeLastLine=*/true);
  return counts;
}

// The arena-backed DP produces byte-identical line breaking vs the vector
// fallback, the arena is actually used, and its peak stays within capacity.
void test_dp_scratch_arena_parity_and_bounded() {
  constexpr int kWords = 400;
  const std::vector<size_t> noArena = layoutLineWordCounts(kWords, nullptr);
  TEST_ASSERT_GREATER_THAN_UINT(1, noArena.size());  // wraps to many lines

  crosspoint::layout::LayoutArena arena = crosspoint::layout::LayoutArena::create(16 * 1024);
  TEST_ASSERT_TRUE(arena.ok());
  const std::vector<size_t> withArena = layoutLineWordCounts(kWords, &arena);

  TEST_ASSERT_EQUAL_UINT(noArena.size(), withArena.size());
  for (size_t i = 0; i < noArena.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT(noArena[i], withArena[i]);
  }
  // dp[] + ans[] = (sizeof(int)+sizeof(size_t)) per word were drawn from the
  // arena, and rewind() returned them (highWater retains the peak).
  TEST_ASSERT_GREATER_OR_EQUAL_UINT(kWords * (sizeof(int) + sizeof(size_t)), arena.highWater());
  TEST_ASSERT_LESS_OR_EQUAL_UINT(arena.capacity(), arena.highWater());
}

// An arena too small for the DP scratch falls back to std::vector cleanly with
// identical output and never partially fills the arena.
void test_dp_scratch_arena_overflow_falls_back() {
  const std::vector<size_t> noArena = layoutLineWordCounts(400, nullptr);
  crosspoint::layout::LayoutArena tiny = crosspoint::layout::LayoutArena::create(64);
  TEST_ASSERT_TRUE(tiny.ok());
  const std::vector<size_t> tinyArena = layoutLineWordCounts(400, &tiny);
  TEST_ASSERT_EQUAL_UINT(noArena.size(), tinyArena.size());
  for (size_t i = 0; i < noArena.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT(noArena[i], tinyArena[i]);
  }
  TEST_ASSERT_EQUAL_UINT(0, tiny.highWater());  // never satisfied a dp/ans alloc
}

// — RFC #164 step 4: golden line-level parity oracle —
//
// The step-3 tests above compare only per-line WORD COUNTS. Step 4 moves the
// word working set (words[]/wordStyles[]/wordContinues[] + the transient
// width/break arrays) into the arena, so it can drift the actual emitted bytes
// and x-positions, not just the break points. These helpers capture the FULL
// emitted line (word strings post soft-hyphen-strip + visible hyphen, x
// positions, styles) so the arena path can be asserted byte-identical to the
// std::vector fallback across the mutating layout cases (justification,
// hyphenation sub-token expansion, oversized-token UTF-8 split, indent,
// continuation runs) — not just plain wrapping.
namespace {

struct WordSpec {
  std::string text;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool attach = false;  // continuation: no space before, attaches to previous
};

struct CapturedLine {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<EpdFontFamily::Style> styles;
  bool oom = false;  // processLine(nullptr) marker
};

// Lay out one paragraph and capture every emitted line in full. `arena`
// (nullable) is passed to layoutAndExtractLines exactly as the parser does.
std::vector<CapturedLine> captureParagraph(const std::vector<WordSpec>& words, bool hyphenation, const BlockStyle& bs,
                                           uint8_t wordSpacing, uint8_t indentMode, bool usePublisher, uint16_t vw,
                                           crosspoint::layout::LayoutArena* arena) {
  ParsedText pt(/*extraSpacing=*/false, hyphenation, bs, wordSpacing, indentMode, usePublisher, arena);
  for (const auto& w : words) pt.addWord(w.text, w.style, /*underline=*/false, w.attach);
  std::vector<CapturedLine> out;
  pt.layoutAndExtractLines(
      g_renderer, 0, vw,
      [&out](std::shared_ptr<TextBlock> tb) {
        if (!tb) {
          out.push_back(CapturedLine{{}, {}, {}, true});
          return;
        }
        out.push_back(CapturedLine{tb->getWords(), tb->getWordXpos(), tb->getWordStyles(), false});
      },
      /*includeLastLine=*/true);
  return out;
}

void assertLineParity(const std::vector<CapturedLine>& a, const std::vector<CapturedLine>& b) {
  TEST_ASSERT_EQUAL_UINT(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    TEST_ASSERT_EQUAL(a[i].oom, b[i].oom);
    TEST_ASSERT_EQUAL_UINT(a[i].words.size(), b[i].words.size());
    TEST_ASSERT_EQUAL_UINT(a[i].xpos.size(), b[i].xpos.size());
    TEST_ASSERT_EQUAL_UINT(a[i].styles.size(), b[i].styles.size());
    for (size_t j = 0; j < a[i].words.size(); ++j) {
      TEST_ASSERT_EQUAL_STRING(a[i].words[j].c_str(), b[i].words[j].c_str());
      TEST_ASSERT_EQUAL_INT16(a[i].xpos[j], b[i].xpos[j]);
      TEST_ASSERT_EQUAL_UINT8(a[i].styles[j], b[i].styles[j]);
    }
  }
}

std::vector<WordSpec> repeatWords(const char* w, int n, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  std::vector<WordSpec> out;
  out.reserve(n);
  for (int i = 0; i < n; ++i) out.push_back(WordSpec{w, style, false});
  return out;
}

// Run the same paragraph with no arena and with a fat arena; assert the full
// emitted output is byte-identical. The arena is sized to comfortably back the
// DP scratch for the case so the arena branch is actually taken.
void expectArenaParity(const std::vector<WordSpec>& words, bool hyphenation, const BlockStyle& bs, uint8_t wordSpacing,
                       uint8_t indentMode, bool usePublisher, uint16_t vw, size_t arenaBytes = 16 * 1024) {
  const std::vector<CapturedLine> noArena =
      captureParagraph(words, hyphenation, bs, wordSpacing, indentMode, usePublisher, vw, nullptr);
  crosspoint::layout::LayoutArena arena = crosspoint::layout::LayoutArena::create(arenaBytes);
  TEST_ASSERT_TRUE(arena.ok());
  const std::vector<CapturedLine> withArena =
      captureParagraph(words, hyphenation, bs, wordSpacing, indentMode, usePublisher, vw, &arena);
  assertLineParity(noArena, withArena);
}

BlockStyle styleWith(CssTextAlign align) {
  BlockStyle bs;
  bs.alignment = align;
  return bs;
}

}  // namespace

void test_layout_parity_plain_wrap() {
  expectArenaParity(repeatWords("lorem", 60), false, styleWith(CssTextAlign::Left), 1, 0, true, 600);
}
void test_layout_parity_justify() {
  expectArenaParity(repeatWords("ipsum", 60), false, styleWith(CssTextAlign::Justify), 1, 0, true, 600);
}
void test_layout_parity_center() {
  expectArenaParity(repeatWords("dolor", 40), false, styleWith(CssTextAlign::Center), 1, 0, true, 600);
}
void test_layout_parity_right() {
  expectArenaParity(repeatWords("amet", 40), false, styleWith(CssTextAlign::Right), 1, 0, true, 600);
}
void test_layout_parity_indent() {
  // firstLineIndentMode=3 forces a 1em first-line indent (applyParagraphIndent).
  expectArenaParity(repeatWords("consectetur", 50), false, styleWith(CssTextAlign::Justify), 1, 3, true, 600);
}
void test_layout_parity_word_spacing_wide() {
  // wordSpacingPercent=3 (Very Wide) exercises the space-width delta path.
  expectArenaParity(repeatWords("elit", 50), false, styleWith(CssTextAlign::Justify), 3, 0, true, 600);
}
void test_layout_parity_hyphenation() {
  // Long words (>8 bytes) drive expandHyphenationBreaks (stub breaks every 4
  // bytes, inserting a visible hyphen at the break) — the lockstep mid-vector
  // insert path step 4 restructures.
  expectArenaParity(repeatWords("internationalization", 30), true, styleWith(CssTextAlign::Justify), 1, 0, true, 600);
}
void test_layout_parity_oversized_token() {
  // A single token far wider than the viewport drives splitOversizedTokens'
  // UTF-8 chunk split + inserts.
  std::vector<WordSpec> w;
  w.push_back(WordSpec{std::string(200, 'M'), EpdFontFamily::REGULAR, false});
  w.push_back(WordSpec{"tail", EpdFontFamily::REGULAR, false});
  expectArenaParity(w, false, styleWith(CssTextAlign::Left), 1, 0, true, 600);
}
void test_layout_parity_continuations() {
  // Continuation words (attach=true) lay out with no gap — exercises the
  // wordContinues path through canBreakBefore + extractLine x-positioning.
  std::vector<WordSpec> w;
  for (int i = 0; i < 30; ++i) {
    w.push_back(WordSpec{"word", EpdFontFamily::REGULAR, false});
    w.push_back(WordSpec{".", EpdFontFamily::REGULAR, true});  // attached punctuation
  }
  expectArenaParity(w, false, styleWith(CssTextAlign::Justify), 1, 0, true, 600);
}
void test_layout_parity_mixed_styles() {
  std::vector<WordSpec> w;
  for (int i = 0; i < 40; ++i) {
    w.push_back(WordSpec{"reg", EpdFontFamily::REGULAR, false});
    w.push_back(WordSpec{"bold", EpdFontFamily::BOLD, false});
    w.push_back(WordSpec{"ital", EpdFontFamily::ITALIC, false});
  }
  expectArenaParity(w, false, styleWith(CssTextAlign::Justify), 1, 0, true, 600);
}

// — bounded-peak / overflow / drain (RFC #164 step 4) —

// Simulate the parser's >750-word drain: accumulate words, flush all-but-last
// line whenever the buffer passes `drainThreshold`, then a final flush. Mirrors
// ChapterHtmlSlimParser's flush loop so consumeArenaPrefix's multi-flush
// lifecycle is exercised.
namespace {
std::vector<CapturedLine> captureWithDrain(const std::vector<WordSpec>& words, int drainThreshold, const BlockStyle& bs,
                                           uint16_t vw, crosspoint::layout::LayoutArena* arena) {
  ParsedText pt(/*extraSpacing=*/false, /*hyphenation=*/false, bs, /*wordSpacing=*/1, /*indentMode=*/0,
                /*usePublisher=*/true, arena);
  std::vector<CapturedLine> out;
  auto emit = [&out](std::shared_ptr<TextBlock> tb) {
    if (!tb) {
      out.push_back(CapturedLine{{}, {}, {}, true});
      return;
    }
    out.push_back(CapturedLine{tb->getWords(), tb->getWordXpos(), tb->getWordStyles(), false});
  };
  for (const auto& w : words) {
    pt.addWord(w.text, w.style, /*underline=*/false, w.attach);
    if (static_cast<int>(pt.size()) > drainThreshold) {
      pt.layoutAndExtractLines(g_renderer, 0, vw, emit, /*includeLastLine=*/false);
    }
  }
  pt.layoutAndExtractLines(g_renderer, 0, vw, emit, /*includeLastLine=*/true);
  return out;
}
}  // namespace

// The arena drain lifecycle (consumeArenaPrefix across many flushes) produces
// the exact same lines as the std::vector path's erase-prefix lifecycle.
void test_arena_drain_lifecycle_parity() {
  const std::vector<WordSpec> w = repeatWords("paragraph", 300);
  const std::vector<CapturedLine> noArena = captureWithDrain(w, 50, styleWith(CssTextAlign::Justify), 600, nullptr);
  crosspoint::layout::LayoutArena arena = crosspoint::layout::LayoutArena::create(16 * 1024);
  TEST_ASSERT_TRUE(arena.ok());
  const std::vector<CapturedLine> withArena = captureWithDrain(w, 50, styleWith(CssTextAlign::Justify), 600, &arena);
  assertLineParity(noArena, withArena);
}

// With the drain bounding the live buffer, the arena peak for a long paragraph
// stays ~equal to a short one and within capacity — the invariant the legacy
// vector path provably could not assert (its peak scaled with paragraph length).
void test_arena_bounded_peak() {
  crosspoint::layout::LayoutArena small = crosspoint::layout::LayoutArena::create(16 * 1024);
  captureWithDrain(repeatWords("paragraph", 200), 50, styleWith(CssTextAlign::Justify), 600, &small);
  const size_t peakShort = small.highWater();

  crosspoint::layout::LayoutArena large = crosspoint::layout::LayoutArena::create(16 * 1024);
  captureWithDrain(repeatWords("paragraph", 1500), 50, styleWith(CssTextAlign::Justify), 600, &large);
  const size_t peakLong = large.highWater();

  // The word buffer was arena-resident (the reserved handle array dominates the
  // peak), and the 7.5x-longer paragraph did not grow the peak materially.
  TEST_ASSERT_GREATER_THAN_UINT(0, peakShort);
  TEST_ASSERT_LESS_OR_EQUAL_UINT(small.capacity(), peakShort);
  TEST_ASSERT_LESS_OR_EQUAL_UINT(large.capacity(), peakLong);
  // Within one DP-scratch slack of each other — bounded, not length-scaled.
  const size_t slack = 2 * 1024;
  TEST_ASSERT_TRUE(peakLong <= peakShort + slack && peakShort <= peakLong + slack);
}

// The arena is reused across blocks: the parser swaps engines via
// reset(new ...) — constructing the next block's ParsedText BEFORE destroying
// the current one. The rewind checkpoint must be captured lazily (first
// addWord), not at construction, or each block's region leaks and the peak
// creeps until the arena exhausts and silently migrates. Drive 30 blocks
// through one arena in that exact order and assert the peak stays flat.
void test_arena_reused_across_blocks_bounded() {
  crosspoint::layout::LayoutArena arena = crosspoint::layout::LayoutArena::create(16 * 1024);
  TEST_ASSERT_TRUE(arena.ok());
  std::unique_ptr<ParsedText> cur;
  size_t peakAfterFirst = 0;
  for (int b = 0; b < 30; ++b) {
    cur.reset(new ParsedText(/*extraSpacing=*/false, /*hyphenation=*/false, BlockStyle(), /*wordSpacing=*/1,
                             /*indentMode=*/0, /*usePublisher=*/true, &arena));  // construct-before-destroy
    for (int i = 0; i < 40; ++i) cur->addWord("word", EpdFontFamily::REGULAR, false, false);
    cur->layoutAndExtractLines(g_renderer, 0, 600, [](std::shared_ptr<TextBlock>) {}, /*includeLastLine=*/true);
    if (b == 0) peakAfterFirst = arena.highWater();
  }
  cur.reset();
  TEST_ASSERT_GREATER_THAN_UINT(0, peakAfterFirst);
  TEST_ASSERT_LESS_OR_EQUAL_UINT(arena.capacity(), arena.highWater());
  // Flat across 30 blocks — region reclaimed each time, not creeping.
  TEST_ASSERT_LESS_OR_EQUAL_UINT(peakAfterFirst + 1024, arena.highWater());
}

// An arena too small to host the handle array migrates to the vector path on
// the first addWord and lays out byte-identically.
void test_arena_too_small_migrates() {
  const std::vector<WordSpec> w = repeatWords("word", 80);
  const std::vector<CapturedLine> noArena =
      captureParagraph(w, false, styleWith(CssTextAlign::Justify), 1, 0, true, 600, nullptr);
  crosspoint::layout::LayoutArena tiny = crosspoint::layout::LayoutArena::create(2 * 1024);  // < handle array
  TEST_ASSERT_TRUE(tiny.ok());
  const std::vector<CapturedLine> withTiny =
      captureParagraph(w, false, styleWith(CssTextAlign::Justify), 1, 0, true, 600, &tiny);
  assertLineParity(noArena, withTiny);
}

// Overflowing the arena's byte region mid-accumulation (handle array fits, but
// the interned bytes do not) migrates the words accumulated so far into the
// vectors and finishes on the vector path — still byte-identical.
void test_arena_byte_overflow_migrates() {
  // Long words so the packed byte region fills before the 1024-handle cap.
  const std::vector<WordSpec> w = repeatWords("supercalifragilistic", 200);
  const std::vector<CapturedLine> noArena =
      captureParagraph(w, false, styleWith(CssTextAlign::Justify), 1, 0, true, 600, nullptr);
  // 13 KB: handle array (~12 KB) fits, leaving ~1 KB for bytes -> interning
  // overflows after a few dozen words -> migrate.
  crosspoint::layout::LayoutArena arena = crosspoint::layout::LayoutArena::create(13 * 1024);
  TEST_ASSERT_TRUE(arena.ok());
  const std::vector<CapturedLine> withArena =
      captureParagraph(w, false, styleWith(CssTextAlign::Justify), 1, 0, true, 600, &arena);
  assertLineParity(noArena, withArena);
}

// — PageBuilder: page-assembly extracted from the parser (crash-hardening:
//   explicit PageStatus replaces the sticky parseFailed bool) —
namespace {
std::shared_ptr<TextBlock> makePageLine() {
  std::vector<std::string> w{"x"};
  std::vector<int16_t> xp{0};
  std::vector<EpdFontFamily::Style> st{EpdFontFamily::REGULAR};
  return std::make_shared<TextBlock>(w, xp, st, BlockStyle());
}
crosspoint::page::PageConfig pbCfg() {
  crosspoint::page::PageConfig c;
  c.viewportWidth = 600;
  c.viewportHeight = 800;
  c.baseLineHeight = 40;  // default BlockStyle resolveLineHeight(40) == 40 -> 20 lines/page
  c.minDensePageLines = 6;
  c.densePageThresholdPercent = 80;
  return c;
}
}  // namespace

// 45 lines at 40px into an 800px viewport => 20+20 emitted during feed, 5 on finish.
void test_pagebuilder_page_boundaries() {
  using namespace crosspoint::page;
  crosspoint::heap::clearLargestFreeBlockOverride();
  FootnotePlacer fp;
  int emitted = 0;
  PageBuilder pb(pbCfg(), fp, [&emitted](std::unique_ptr<Page>) { ++emitted; }, [](const std::string&, uint16_t) {});
  for (int i = 0; i < 45; ++i) {
    TEST_ASSERT_TRUE(ok(pb.addLine(makePageLine())));
  }
  TEST_ASSERT_EQUAL_UINT(2, pb.completedPageCount());  // two full pages emitted during the feed
  pb.finish();                                         // trailing 5-line page
  TEST_ASSERT_EQUAL_UINT(3, pb.completedPageCount());
  TEST_ASSERT_EQUAL_INT(3, emitted);
}

// The crash-hardening invariant: a heap-probe failure mid-page returns Oom
// explicitly (was a silently-polled sticky flag) and emits nothing.
void test_pagebuilder_oom_probe_is_explicit() {
  using namespace crosspoint::page;
  FootnotePlacer fp;
  int emitted = 0;
  PageBuilder pb(pbCfg(), fp, [&emitted](std::unique_ptr<Page>) { ++emitted; }, [](const std::string&, uint16_t) {});
  crosspoint::heap::setLargestFreeBlockOverride(8);  // too small for the PageLine probe
  const PageStatus s = pb.addLine(makePageLine());
  crosspoint::heap::clearLargestFreeBlockOverride();
  TEST_ASSERT_TRUE(s == PageStatus::Oom);
  TEST_ASSERT_EQUAL_INT(0, emitted);
}

// A null line (upstream LayoutEngine OOM) returns Oom, not a silent skip.
void test_pagebuilder_null_line_is_oom() {
  using namespace crosspoint::page;
  crosspoint::heap::clearLargestFreeBlockOverride();
  FootnotePlacer fp;
  PageBuilder pb(pbCfg(), fp, [](std::unique_ptr<Page>) {}, [](const std::string&, uint16_t) {});
  TEST_ASSERT_TRUE(pb.addLine(nullptr) == PageStatus::Oom);
}

// RFC #171-followup parity tests: the behaviours the parser relies on that the
// original PageBuilder tests did not cover (the wiring would silently regress
// without these).

// ensureOpenPage() before advanceY() preserves a fresh page's top spacing —
// the parser applies a paragraph's top margin before its first line. Without
// the seam, addLine's own ensurePage would reset the cursor and drop the margin.
void test_pagebuilder_top_spacing_survives_fresh_page() {
  using namespace crosspoint::page;
  crosspoint::heap::clearLargestFreeBlockOverride();
  FootnotePlacer fp;
  PageBuilder pb(pbCfg(), fp, [](std::unique_ptr<Page>) {}, [](const std::string&, uint16_t) {});
  TEST_ASSERT_TRUE(ok(pb.ensureOpenPage()));
  pb.advanceY(100);  // paragraph top margin on a fresh page
  TEST_ASSERT_EQUAL_INT(100, pb.cursorY());
  TEST_ASSERT_TRUE(ok(pb.addLine(makePageLine())));  // baseLineHeight 40
  TEST_ASSERT_EQUAL_INT(140, pb.cursorY());          // line landed at y=100, NOT reset to 0
}

// finish() must NOT emit a trailing empty page (e.g. left open by a final
// ensureOpenPage on an empty paragraph) — that would inflate the page count.
void test_pagebuilder_finish_skips_empty_page() {
  using namespace crosspoint::page;
  crosspoint::heap::clearLargestFreeBlockOverride();
  FootnotePlacer fp;
  int emitted = 0;
  PageBuilder pb(pbCfg(), fp, [&emitted](std::unique_ptr<Page>) { ++emitted; }, [](const std::string&, uint16_t) {});
  TEST_ASSERT_TRUE(ok(pb.ensureOpenPage()));  // open an empty page
  pb.finish();
  TEST_ASSERT_EQUAL_UINT(0, pb.completedPageCount());
  TEST_ASSERT_EQUAL_INT(0, emitted);
}

// Trailing anchors bind to the last non-empty page; an anchor on an empty page
// stays unbound (matches the old parse-end guard).
void test_pagebuilder_trailing_anchors() {
  using namespace crosspoint::page;
  crosspoint::heap::clearLargestFreeBlockOverride();
  FootnotePlacer fp;
  std::vector<std::pair<std::string, uint16_t>> binds;
  PageBuilder pb(
      pbCfg(), fp, [](std::unique_ptr<Page>) {},
      [&binds](const std::string& a, uint16_t p) { binds.emplace_back(a, p); });
  pb.queueAnchors({"a1"});
  TEST_ASSERT_TRUE(ok(pb.addLine(makePageLine())));  // a1 binds to page 0 as the line lands
  pb.queueAnchors({"a2"});
  TEST_ASSERT_TRUE(ok(pb.bindTrailingAnchors()));  // a2 binds to the same non-empty page 0
  TEST_ASSERT_EQUAL_UINT(2, binds.size());
  TEST_ASSERT_EQUAL_STRING("a1", binds[0].first.c_str());
  TEST_ASSERT_EQUAL_STRING("a2", binds[1].first.c_str());

  // A trailing anchor on a fresh/empty builder is NOT bound.
  PageBuilder pb2(
      pbCfg(), fp, [](std::unique_ptr<Page>) {},
      [&binds](const std::string& a, uint16_t p) { binds.emplace_back(a, p); });
  pb2.queueAnchors({"a3"});
  TEST_ASSERT_TRUE(ok(pb2.bindTrailingAnchors()));
  TEST_ASSERT_EQUAL_UINT(2, binds.size());  // unchanged: a3 not bound
}

// addImage page-breaks only when the current page is non-empty and the image
// won't fit; a too-tall image on a fresh page is kept.
void test_pagebuilder_image_break() {
  using namespace crosspoint::page;
  crosspoint::heap::clearLargestFreeBlockOverride();
  FootnotePlacer fp;
  int emitted = 0;
  PageBuilder pb(pbCfg(), fp, [&emitted](std::unique_ptr<Page>) { ++emitted; }, [](const std::string&, uint16_t) {});
  auto img = std::make_shared<ImageBlock>("/x.bmp", 600, 100);
  for (int i = 0; i < 19; ++i) TEST_ASSERT_TRUE(ok(pb.addLine(makePageLine())));  // cursor 760/800
  TEST_ASSERT_TRUE(ok(pb.addImage(img, 600, 100)));                               // 760+100>800 + non-empty -> break
  TEST_ASSERT_EQUAL_UINT(1, pb.completedPageCount());
  TEST_ASSERT_EQUAL_INT(100, pb.cursorY());  // image at y=0 on the new page, cursor=100
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pagebuilder_page_boundaries);
  RUN_TEST(test_pagebuilder_oom_probe_is_explicit);
  RUN_TEST(test_pagebuilder_null_line_is_oom);
  RUN_TEST(test_pagebuilder_top_spacing_survives_fresh_page);
  RUN_TEST(test_pagebuilder_finish_skips_empty_page);
  RUN_TEST(test_pagebuilder_trailing_anchors);
  RUN_TEST(test_pagebuilder_image_break);
  RUN_TEST(test_dp_scratch_arena_parity_and_bounded);
  RUN_TEST(test_dp_scratch_arena_overflow_falls_back);
  RUN_TEST(test_layout_parity_plain_wrap);
  RUN_TEST(test_layout_parity_justify);
  RUN_TEST(test_layout_parity_center);
  RUN_TEST(test_layout_parity_right);
  RUN_TEST(test_layout_parity_indent);
  RUN_TEST(test_layout_parity_word_spacing_wide);
  RUN_TEST(test_layout_parity_hyphenation);
  RUN_TEST(test_layout_parity_oversized_token);
  RUN_TEST(test_layout_parity_continuations);
  RUN_TEST(test_layout_parity_mixed_styles);
  RUN_TEST(test_arena_drain_lifecycle_parity);
  RUN_TEST(test_arena_bounded_peak);
  RUN_TEST(test_arena_reused_across_blocks_bounded);
  RUN_TEST(test_arena_too_small_migrates);
  RUN_TEST(test_arena_byte_overflow_migrates);
  RUN_TEST(test_parse_chapter_healthy);
  RUN_TEST(test_parse_chapter_under_fragmentation);
  RUN_TEST(test_style_plain_is_regular);
  RUN_TEST(test_style_bold_tag);
  RUN_TEST(test_style_em_then_bold_nested);
  RUN_TEST(test_style_css_base_italic);
  RUN_TEST(test_style_underline_via_anchor);
  RUN_TEST(test_style_inline_normal_unbolds_under_depth_flag);
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
