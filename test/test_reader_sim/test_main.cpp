// Host reader-sim — phase 2 slice: drive the REAL ParsedText layout engine on
// the host, under SimHeap allocation-failure injection, to prove the layout
// hot path is memory-stable (no unguarded allocation would abort the firmware)
// and to measure layout time (snappiness baseline).
//
// This exercises production code (lib/Epub/Epub/ParsedText.cpp) — the words[]
// vector growth + the canAllocateContiguous OOM probe + computeLineBreaks —
// which is exactly where the on-device OOM crashes originate.
//
// Run via: pio test -e test_sim -f test_reader_sim
#include <unity.h>

#include <GfxRenderer.h>  // shadow: full definition needed for g_renderer instance
#include <HeapGuard.h>
#include <ParsedText.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "../test_sim_heap/SimHeap.h"

using crosspoint::test::SimHeap;

namespace {

GfxRenderer g_renderer;  // shadow, metrics-only

// Build a paragraph of `words` words and lay it out at `viewportWidth` px.
// `longEvery`>0 injects an oversized unbreakable token every Nth word (drives
// splitOversizedTokens). `hyphenate` enables the hyphenation expansion path.
// Returns the number of lines produced via the processLine callback.
uint32_t layoutParagraph(int words, int viewportWidth, int longEvery = 0, bool hyphenate = false) {
  ParsedText pt(/*extraParagraphSpacing=*/false, /*hyphenationEnabled=*/hyphenate);
  for (int i = 0; i < words; ++i) {
    if (longEvery > 0 && (i % longEvery) == 0) {
      pt.addWord(std::string(48, 'm'), EpdFontFamily::REGULAR);  // oversized token
    } else {
      pt.addWord(std::string("word") + std::to_string(i % 97), EpdFontFamily::REGULAR);
    }
  }
  uint32_t lines = 0;
  pt.layoutAndExtractLines(g_renderer, /*fontId=*/0, static_cast<uint16_t>(viewportWidth),
                           [&lines](std::shared_ptr<TextBlock>) { ++lines; },
                           /*includeLastLine=*/true);
  return lines;
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

// Healthy heap: a normal paragraph lays out into multiple lines, no OOM.
void test_layout_healthy_produces_lines() {
  const uint32_t lines = layoutParagraph(/*words=*/200, /*viewportWidth=*/600);
  TEST_ASSERT_GREATER_THAN_UINT(0, lines);
}

// THE STABILITY ASSERTION: under the v3.0.1 user's fragmentation (largest
// block 11764 of 142824 total), the layout must NOT reach a throwing
// allocation. The addWord heap probe (HeapGuard) should bail cleanly; SimHeap
// confirms no unguarded allocation would have aborted the firmware.
void test_layout_under_fragmentation_no_would_abort() {
  constexpr size_t kCap = 11764;
  constexpr size_t kBudget = 142824;

  // The probe-based guards read HeapGuard; the real allocations are policed by
  // SimHeap. Drive both from the same fragmentation state.
  crosspoint::heap::setLargestFreeBlockOverride(kCap);
  SimHeap::arm(kCap, kBudget);

  bool threw = false;
  uint32_t lines = 0;
  try {
    lines = layoutParagraph(/*words=*/500, /*viewportWidth=*/600);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();

  SimHeap::disarm();
  crosspoint::heap::clearLargestFreeBlockOverride();

  TEST_ASSERT_FALSE(threw);                     // no throwing alloc reached
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);        // no unguarded alloc would crash
  (void)lines;  // layout may have bailed early via oom_ — that's the safe path
}

// Snappiness + memory-churn baseline. Measures layout wall-clock AND the number
// of heap allocations layout performs (allocation churn is both a stability
// signal — fewer allocs fragment less — and a snappiness signal — malloc has
// real cost). Surfaced via TEST_MESSAGE (Unity captures stdout). Not a pass/fail
// gate yet; it's the reference an optimization is compared against.
void test_layout_churn_and_timing_baseline() {
  // Arm with a huge cap+budget so nothing FAILS — we only want SimHeap to COUNT
  // the allocations the layout performs.
  SimHeap::arm(/*cap=*/8u * 1024 * 1024, /*budget=*/64u * 1024 * 1024);
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t lines = layoutParagraph(/*words=*/300, /*viewportWidth=*/600);
  const auto t1 = std::chrono::steady_clock::now();
  const unsigned allocs = SimHeap::attempts();
  const size_t peak = SimHeap::peakLiveBytes();
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();

  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  char msg[160];
  snprintf(msg, sizeof(msg), "BASELINE layout(300w): %lld us | %u allocs | %zu B peak | %u lines",
           static_cast<long long>(us), allocs, peak, lines);
  TEST_MESSAGE(msg);

  TEST_ASSERT_GREATER_THAN_UINT(0, lines);
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);  // baseline run must itself be clean
}

// Oversized-token path under fragmentation. Long unbreakable tokens at a narrow
// viewport force ParsedText::splitOversizedTokens (UTF-8 chunk split) — one of
// the v3.0.1 bad_alloc hotfix sites. The probe must bail cleanly; no throwing
// alloc may reach abort.
void test_oversized_tokens_under_fragmentation_no_would_abort() {
  constexpr size_t kCap = 9 * 1024;
  crosspoint::heap::setLargestFreeBlockOverride(kCap);
  SimHeap::arm(kCap, 142824);
  bool threw = false;
  try {
    layoutParagraph(/*words=*/300, /*viewportWidth=*/200, /*longEvery=*/7, /*hyphenate=*/false);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();
  crosspoint::heap::clearLargestFreeBlockOverride();
  TEST_ASSERT_FALSE(threw);
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);
}

// Hyphenation-expansion path under fragmentation (expandHyphenationBreaks — the
// other v3.0.1 hotfix site). Synthetic hyphenator inserts sub-token breaks.
void test_hyphenation_path_under_fragmentation_no_would_abort() {
  constexpr size_t kCap = 9 * 1024;
  crosspoint::heap::setLargestFreeBlockOverride(kCap);
  SimHeap::arm(kCap, 142824);
  bool threw = false;
  try {
    layoutParagraph(/*words=*/300, /*viewportWidth=*/220, /*longEvery=*/5, /*hyphenate=*/true);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const unsigned wouldAbort = SimHeap::wouldAbortThrows();
  SimHeap::disarm();
  crosspoint::heap::clearLargestFreeBlockOverride();
  TEST_ASSERT_FALSE(threw);
  TEST_ASSERT_EQUAL_UINT(0, wouldAbort);
}

// Fuzz: many layouts across the full fragmentation spectrum (1 KB .. 90 KB
// largest-block), varying word counts / oversized tokens / hyphenation. The
// contract mirrors the firmware's: under ANY fragmentation, layout either
// completes or bails via the oom_ probe — it must NEVER reach a throwing
// allocation (which on-device aborts -> RTC_SW_SYS_RST). Deterministic seed.
void test_fuzz_layout_random_fragmentation_never_aborts() {
  const size_t bins[] = {1024, 4096, 9216, 12288, 25600, 49152, 90000};
  uint32_t lcg = 0xBEEF1234u;
  auto next = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg; };

  unsigned totalWouldAbort = 0;
  bool anyThrew = false;
  for (int iter = 0; iter < 300; ++iter) {
    const size_t cap = bins[next() % 7];
    const int words = 20 + static_cast<int>(next() % 400);
    const int viewport = 160 + static_cast<int>(next() % 480);
    const int longEvery = (next() % 3 == 0) ? (3 + static_cast<int>(next() % 9)) : 0;
    const bool hyph = (next() % 2) == 0;

    crosspoint::heap::setLargestFreeBlockOverride(cap);
    SimHeap::arm(cap, 142824);
    try {
      layoutParagraph(words, viewport, longEvery, hyph);
    } catch (const std::bad_alloc&) {
      anyThrew = true;
    }
    totalWouldAbort += SimHeap::wouldAbortThrows();
    SimHeap::disarm();
    crosspoint::heap::clearLargestFreeBlockOverride();
  }

  char msg[96];
  snprintf(msg, sizeof(msg), "FUZZ 300 layouts across 1KB..90KB frag: wouldAbort=%u", totalWouldAbort);
  TEST_MESSAGE(msg);
  TEST_ASSERT_FALSE(anyThrew);
  TEST_ASSERT_EQUAL_UINT(0, totalWouldAbort);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_layout_healthy_produces_lines);
  RUN_TEST(test_layout_under_fragmentation_no_would_abort);
  RUN_TEST(test_oversized_tokens_under_fragmentation_no_would_abort);
  RUN_TEST(test_hyphenation_path_under_fragmentation_no_would_abort);
  RUN_TEST(test_fuzz_layout_random_fragmentation_never_aborts);
  RUN_TEST(test_layout_churn_and_timing_baseline);
  return UNITY_END();
}
