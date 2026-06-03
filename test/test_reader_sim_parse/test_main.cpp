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
  ChapterHtmlSlimParser parser(epub, filepath, g_renderer, /*fontId=*/0, /*lineCompression=*/1.0f,
                               /*extraParagraphSpacingLevel=*/0, /*paragraphAlignment=*/0,
                               /*viewportWidth=*/600, /*viewportHeight=*/800, /*hyphenationEnabled=*/false,
                               /*wordSpacingPercent=*/1, /*firstLineIndentMode=*/0, /*usePublisherStyles=*/true,
                               [&pages](std::unique_ptr<Page>) { ++pages; },
                               [](const std::string&, uint16_t) {}, /*contentBase=*/"", /*imageBasePath=*/"",
                               /*progressFn=*/nullptr, /*cssParser=*/nullptr);
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
  return UNITY_END();
}
