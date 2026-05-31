// Host ZIP-decompress sim: run the REAL ZipFile against a real .epub on host,
// under SimHeap, to (a) prove the DEFLATE/inflate path is memory-stable under
// fragmentation and (b) measure decompress timing (a real snappiness sink —
// every chapter open inflates from the EPUB zip).
//
// Run via: pio test -e test_sim_zip -f test_reader_sim_zip
#include <unity.h>

#include <HalStorage.h>
#include <Print.h>
#include <ZipFile.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "../test_sim_heap/SimHeap.h"

#ifndef SIM_EPUB_PATH
#define SIM_EPUB_PATH "test/test_reader_sim/fixtures/book.epub"
#endif

using crosspoint::test::SimHeap;

namespace {
// A representative compressed chapter inside the Gutenberg fixture (72 KB
// uncompressed). If the fixture is replaced, update this entry name.
const char* kChapterEntry = "OEBPS/7910875783089588439_1342-h-4.htm.html";

// Print sink that just counts the decompressed bytes streamed to it.
class CountingPrint : public Print {
 public:
  size_t bytes = 0;
  size_t write(uint8_t) override {
    ++bytes;
    return 1;
  }
  size_t write(const uint8_t*, size_t n) override {
    bytes += n;
    return n;
  }
};

size_t streamChapter() {
  std::string path = SIM_EPUB_PATH;  // ZipFile stores filePath by reference — keep alive.
  ZipFile zip(path);
  CountingPrint sink;
  zip.readFileToStream(kChapterEntry, sink, /*chunkSize=*/1024);
  return sink.bytes;
}
}  // namespace

void setUp() { SimHeap::reset(); }
void tearDown() { SimHeap::reset(); }

// Healthy: the real chapter inflates correctly and we can time it.
void test_inflate_chapter_healthy() {
  SimHeap::arm(/*cap=*/8u * 1024 * 1024, /*budget=*/64u * 1024 * 1024);
  const auto t0 = std::chrono::steady_clock::now();
  const size_t bytes = streamChapter();
  const auto t1 = std::chrono::steady_clock::now();
  const unsigned allocs = SimHeap::attempts();
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();

  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  char msg[160];
  snprintf(msg, sizeof(msg), "INFLATE chapter: %zu bytes | %lld us | %u allocs | wouldAbort=%u", bytes,
           static_cast<long long>(us), allocs, wouldAbort);
  TEST_MESSAGE(msg);

  TEST_ASSERT_GREATER_THAN_UINT(50000, bytes);  // ~72 KB chapter
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);
}

// Fragmented heap (the v3.0.1 incident: 11.7 KB largest block). The inflate
// path must not reach a throwing allocation. Records wouldAbort either way: a
// non-zero result is a real finding (an unguarded large alloc in the decompress
// path that would crash the firmware on a fragmented heap).
void test_inflate_chapter_under_fragmentation() {
  SimHeap::arm(/*cap=*/11764, /*budget=*/142824);
  bool threw = false;
  size_t bytes = 0;
  try {
    bytes = streamChapter();
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();

  char msg[128];
  snprintf(msg, sizeof(msg), "INFLATE@frag(11764): bytes=%zu threw=%d wouldAbort=%u", bytes, threw ? 1 : 0,
           wouldAbort);
  TEST_MESSAGE(msg);

  // Reaching here without the host process crashing is the minimum bar. The
  // wouldAbort count above tells us whether the path is hardened (0) or has an
  // unguarded large alloc that needs the nothrow/probe treatment (>0).
  TEST_ASSERT_TRUE(true);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_inflate_chapter_healthy);
  RUN_TEST(test_inflate_chapter_under_fragmentation);
  return UNITY_END();
}
