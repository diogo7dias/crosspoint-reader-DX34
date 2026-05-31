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

  TEST_ASSERT_GREATER_THAN_UINT(0, pages);
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_chapter_healthy);
  RUN_TEST(test_parse_chapter_under_fragmentation);
  return UNITY_END();
}
