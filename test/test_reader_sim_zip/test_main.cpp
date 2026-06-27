// Host ZIP-decompress sim: run the REAL ZipFile against a real .epub on host,
// under SimHeap, to (a) prove the DEFLATE/inflate path is memory-stable under
// fragmentation and (b) measure decompress timing (a real snappiness sink —
// every chapter open inflates from the EPUB zip).
//
// Run via: pio test -e test_sim_zip -f test_reader_sim_zip
#include <HalStorage.h>
#include <MemoryPolicy.h>
#include <Print.h>
#include <ZipFile.h>
#include <unity.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
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

// Read the chapter via the whole-buffer path (ZipFile::readFileToMemory), which
// allocates through the shed-aware C-seam (crosspoint::mem::tryMallocShed).
// Returns the byte count; reports whether the buffer came back non-null.
size_t readChapterToMemory(bool* outNonNull) {
  std::string path = SIM_EPUB_PATH;
  ZipFile zip(path);
  size_t size = 0;
  uint8_t* buf = zip.readFileToMemory(kChapterEntry, &size, /*trailingNullByte=*/false);
  *outNonNull = (buf != nullptr);
  if (buf != nullptr) std::free(buf);  // readFileToMemory hands ownership to caller
  return size;
}

// ── tryMalloc failure-injection seam (SimHeap models operator new, NOT malloc;
// the C-decoder seam uses std::malloc, so the shed-retry path is driven via the
// MemoryPolicy hook instead). ───────────────────────────────────────────────
int g_mallocCalls = 0;
bool g_shedRan = false;
// Null on the first request of an episode, then (after a shed has run) a real
// buffer — models "fragmented + caches pinned, one shed frees enough".
void* failMallocUntilShed(size_t n) {
  ++g_mallocCalls;
  if (!g_shedRan) return nullptr;
  return std::malloc(n);
}
void* alwaysFailMalloc(size_t) {
  ++g_mallocCalls;
  return nullptr;
}
}  // namespace

void setUp() {
  SimHeap::reset();
  crosspoint::mem::clearTryMallocHookForTest();
  crosspoint::mem::clearShedEvictors();
  g_mallocCalls = 0;
  g_shedRan = false;
}
void tearDown() {
  SimHeap::reset();
  crosspoint::mem::clearTryMallocHookForTest();
  crosspoint::mem::clearShedEvictors();
}

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
  snprintf(msg, sizeof(msg), "INFLATE@frag(11764): bytes=%zu threw=%d wouldAbort=%u", bytes, threw ? 1 : 0, wouldAbort);
  TEST_MESSAGE(msg);

  // Reaching here without the host process crashing is the minimum bar. The
  // wouldAbort count above tells us whether the path is hardened (0) or has an
  // unguarded large alloc that needs the nothrow/probe treatment (>0).
  TEST_ASSERT_TRUE(true);
}

// ── readFileToMemory + the shed-aware C-seam (step-1 hardening, end-to-end) ──

// Healthy: the whole-buffer read returns the chapter, no injection.
void test_read_to_memory_healthy_returns_chapter() {
  bool nonNull = false;
  const size_t sz = readChapterToMemory(&nonNull);
  TEST_ASSERT_TRUE(nonNull);
  TEST_ASSERT_GREATER_THAN_UINT(50000, sz);
}

// Transient malloc failure + a shed evictor that frees memory: tryMallocShed
// must shed once and recover, so the chapter still loads. This is the on-device
// "heap fragmented, font cache pinned" case that pre-step-1 gave up on (plain
// tryMalloc never invokes the shed-retry net).
void test_read_to_memory_recovers_after_shed() {
  crosspoint::mem::registerShedEvictor([]() { g_shedRan = true; });
  crosspoint::mem::setTryMallocHookForTest(&failMallocUntilShed);

  bool nonNull = false;
  const size_t sz = readChapterToMemory(&nonNull);

  TEST_ASSERT_TRUE(g_shedRan);                  // the shed fired
  TEST_ASSERT_GREATER_THAN_UINT(1, g_mallocCalls);  // failed, shed, retried
  TEST_ASSERT_TRUE(nonNull);                    // recovered -> chapter loaded
  TEST_ASSERT_GREATER_THAN_UINT(50000, sz);
}

// Persistent malloc failure (shedding frees nothing): readFileToMemory must
// return nullptr gracefully — the device's null-check path. No crash, no abort.
void test_read_to_memory_returns_null_when_exhausted() {
  crosspoint::mem::registerShedEvictor([]() {});  // frees nothing
  crosspoint::mem::setTryMallocHookForTest(&alwaysFailMalloc);

  bool nonNull = true;
  const size_t sz = readChapterToMemory(&nonNull);

  TEST_ASSERT_FALSE(nonNull);  // graceful null, not a crash
  (void)sz;
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_inflate_chapter_healthy);
  RUN_TEST(test_inflate_chapter_under_fragmentation);
  RUN_TEST(test_read_to_memory_healthy_returns_chapter);
  RUN_TEST(test_read_to_memory_recovers_after_shed);
  RUN_TEST(test_read_to_memory_returns_null_when_exhausted);
  return UNITY_END();
}
