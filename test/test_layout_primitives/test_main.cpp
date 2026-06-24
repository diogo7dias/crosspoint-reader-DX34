// Host tests for the RFC #164 layout primitives: the DegradeLevel/DegradePlan
// vocabulary and the bounded LayoutArena. Pure, no hardware:
//   pio test -e test_host -f test_layout_primitives
#include <unity.h>

#include <cstdint>
#include <string>

#include "../../src/activities/reader/ReaderInkCentering.h"
#include "layout/DegradeLevel.h"
#include "layout/LayoutArena.h"

using crosspoint::layout::DegradeLevel;
using crosspoint::layout::DegradePlan;
using crosspoint::layout::LayoutArena;
using crosspoint::reader::inkCenterOffset;
namespace L = crosspoint::layout;

void setUp() {}
void tearDown() {}

// ── DegradePlan ──────────────────────────────────────────────────────────────

void test_degrade_full_keeps_everything() {
  const DegradePlan p = DegradePlan::from(DegradeLevel::Full, L::kStyleAll);
  TEST_ASSERT_TRUE(p.images);
  TEST_ASSERT_TRUE(p.hyphenate);
  TEST_ASSERT_TRUE(p.optimalBreak);
  TEST_ASSERT_EQUAL_UINT8(L::kStyleAll, p.prewarmStyleMask);
}

void test_degrade_levels_shed_in_order() {
  const DegradePlan trim = DegradePlan::from(DegradeLevel::TrimPrewarm, L::kStyleAll);
  TEST_ASSERT_TRUE(trim.optimalBreak);  // layout fidelity untouched
  TEST_ASSERT_TRUE(trim.hyphenate);
  TEST_ASSERT_TRUE(trim.images);
  TEST_ASSERT_EQUAL_UINT8(L::kStyleRegular, trim.prewarmStyleMask);

  const DegradePlan simple = DegradePlan::from(DegradeLevel::SimpleBreak, L::kStyleAll);
  TEST_ASSERT_FALSE(simple.optimalBreak);  // greedy now
  TEST_ASSERT_TRUE(simple.hyphenate);

  const DegradePlan noHy = DegradePlan::from(DegradeLevel::NoHyphen, L::kStyleAll);
  TEST_ASSERT_FALSE(noHy.optimalBreak);
  TEST_ASSERT_FALSE(noHy.hyphenate);
  TEST_ASSERT_TRUE(noHy.images);

  const DegradePlan skip = DegradePlan::from(DegradeLevel::SkipImages, L::kStyleAll);
  TEST_ASSERT_FALSE(skip.images);
  TEST_ASSERT_FALSE(skip.hyphenate);
  TEST_ASSERT_FALSE(skip.optimalBreak);
}

void test_degrade_is_monotone() {
  const DegradeLevel order[] = {DegradeLevel::Full, DegradeLevel::TrimPrewarm, DegradeLevel::SimpleBreak,
                                DegradeLevel::NoHyphen, DegradeLevel::SkipImages};
  DegradePlan prev = DegradePlan::from(order[0], L::kStyleAll);
  for (int i = 1; i < 5; ++i) {
    const DegradePlan cur = DegradePlan::from(order[i], L::kStyleAll);
    // A flag may only go true->false as the level rises (cur implies prev).
    TEST_ASSERT_TRUE(!cur.images || prev.images);
    TEST_ASSERT_TRUE(!cur.hyphenate || prev.hyphenate);
    TEST_ASSERT_TRUE(!cur.optimalBreak || prev.optimalBreak);
    // Prewarm mask is a subset of the previous level's mask.
    TEST_ASSERT_EQUAL_UINT8(cur.prewarmStyleMask, cur.prewarmStyleMask & prev.prewarmStyleMask);
    prev = cur;
  }
}

void test_prewarm_mask_intersects_used_styles() {
  // Full warms exactly the styles the page uses.
  const DegradePlan p = DegradePlan::from(DegradeLevel::Full, L::kStyleRegular | L::kStyleBold);
  TEST_ASSERT_EQUAL_UINT8(L::kStyleRegular | L::kStyleBold, p.prewarmStyleMask);
  // A degraded level trims to regular-only, intersected with used: a page that
  // never uses regular warms nothing.
  const DegradePlan t = DegradePlan::from(DegradeLevel::TrimPrewarm, L::kStyleBold | L::kStyleItalic);
  TEST_ASSERT_EQUAL_UINT8(0, t.prewarmStyleMask);
}

// ── Heap-pressure -> level mapping (RFC #164 step 7, Tier A) ─────────────────

void test_layout_level_bands() {
  // noHyphenBelow=48K, skipImagesBelow=28K (the Section.cpp wiring values).
  const size_t noHy = 48 * 1024, skip = 28 * 1024;
  // Comfortable heap -> Full (keep hyphenation + images).
  TEST_ASSERT_EQUAL(DegradeLevel::Full, L::layoutLevelFor(64 * 1024, noHy, skip));
  TEST_ASSERT_EQUAL(DegradeLevel::Full, L::layoutLevelFor(48 * 1024, noHy, skip));  // boundary is inclusive Full
  // Squeezed -> drop hyphenation first (the cheaper lever).
  TEST_ASSERT_EQUAL(DegradeLevel::NoHyphen, L::layoutLevelFor(48 * 1024 - 1, noHy, skip));
  TEST_ASSERT_EQUAL(DegradeLevel::NoHyphen, L::layoutLevelFor(28 * 1024, noHy, skip));
  // Severe -> also skip images (the largest single decode buffer).
  TEST_ASSERT_EQUAL(DegradeLevel::SkipImages, L::layoutLevelFor(28 * 1024 - 1, noHy, skip));
  TEST_ASSERT_EQUAL(DegradeLevel::SkipImages, L::layoutLevelFor(0, noHy, skip));
}

void test_render_level_bands() {
  const size_t trim = 40 * 1024;
  TEST_ASSERT_EQUAL(DegradeLevel::Full, L::renderLevelFor(40 * 1024, trim));
  TEST_ASSERT_EQUAL(DegradeLevel::Full, L::renderLevelFor(64 * 1024, trim));
  TEST_ASSERT_EQUAL(DegradeLevel::TrimPrewarm, L::renderLevelFor(40 * 1024 - 1, trim));
  TEST_ASSERT_EQUAL(DegradeLevel::TrimPrewarm, L::renderLevelFor(0, trim));
}

void test_layout_level_is_monotone_in_heap() {
  // As the largest block shrinks, the level only ever sheds more (never less).
  const size_t noHy = 48 * 1024, skip = 28 * 1024;
  uint8_t prev = 0;
  for (size_t largest = 80 * 1024; largest > 0; largest -= 512) {
    const uint8_t lvl = static_cast<uint8_t>(L::layoutLevelFor(largest, noHy, skip));
    TEST_ASSERT_TRUE(lvl >= prev);  // monotone non-decreasing as heap falls
    prev = lvl;
  }
}

// ── LayoutArena ──────────────────────────────────────────────────────────────

void test_arena_create_ok_and_zero() {
  LayoutArena z = LayoutArena::create(0);
  TEST_ASSERT_FALSE(z.ok());

  LayoutArena a = LayoutArena::create(1024);
  TEST_ASSERT_TRUE(a.ok());
  TEST_ASSERT_EQUAL_size_t(1024, a.capacity());
  TEST_ASSERT_EQUAL_size_t(0, a.highWater());
}

void test_arena_alloc_aligns_and_overflows() {
  LayoutArena a = LayoutArena::create(256);
  int* p = a.alloc<int>(10);
  TEST_ASSERT_NOT_NULL(p);
  p[0] = 1;
  p[9] = 9;  // writable across the span
  TEST_ASSERT_TRUE(a.highWater() >= 40);

  TEST_ASSERT_NULL(a.alloc<int>(0));      // degenerate
  TEST_ASSERT_NULL(a.alloc<int>(10000));  // 40000 > 256 -> overflow, no abort

  // Alignment: a misaligning char alloc must not break a following double.
  LayoutArena b = LayoutArena::create(256);
  b.alloc<char>(1);
  double* d = b.alloc<double>(2);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL_size_t(0, reinterpret_cast<uintptr_t>(d) % alignof(double));
}

void test_arena_intern_roundtrip_and_overflow() {
  LayoutArena a = LayoutArena::create(64);
  const LayoutArena::Str h = a.intern("hello", 5);
  TEST_ASSERT_TRUE(h.valid());
  TEST_ASSERT_EQUAL_UINT16(5, h.len);
  TEST_ASSERT_EQUAL_STRING("hello", a.str(h));

  const LayoutArena::Str e = a.intern("", 0);
  TEST_ASSERT_TRUE(e.valid());
  TEST_ASSERT_EQUAL_STRING("", a.str(e));

  const std::string big(200, 'x');
  const LayoutArena::Str overflow = a.intern(big.c_str(), big.size());
  TEST_ASSERT_FALSE(overflow.valid());
  TEST_ASSERT_EQUAL_STRING("", a.str(overflow));  // invalid handle -> ""
}

void test_arena_alloc_and_intern_collide_cleanly() {
  LayoutArena a = LayoutArena::create(32);
  char* c = a.alloc<char>(20);
  TEST_ASSERT_NOT_NULL(c);
  // 20 bytes + NUL = 21 cannot fit in the remaining 12.
  const LayoutArena::Str h = a.intern("aaaaaaaaaaaaaaaaaaaa", 20);
  TEST_ASSERT_FALSE(h.valid());
  // A small word still fits in the gap.
  const LayoutArena::Str s = a.intern("ab", 2);
  TEST_ASSERT_TRUE(s.valid());
  TEST_ASSERT_EQUAL_STRING("ab", a.str(s));
}

void test_arena_mark_rewind_reclaims() {
  LayoutArena a = LayoutArena::create(128);
  const LayoutArena::Mark m = a.mark();
  a.alloc<int>(10);             // 40 from the front
  a.intern("hello world", 11);  // 12 from the back
  TEST_ASSERT_TRUE(a.highWater() >= 52);

  a.rewind(m);
  // Full capacity is available again — an 80-byte alloc that would not fit
  // alongside the prior 52 now succeeds.
  int* p = a.alloc<int>(20);
  TEST_ASSERT_NOT_NULL(p);
}

void test_arena_highwater_not_lowered_by_rewind() {
  LayoutArena a = LayoutArena::create(128);
  const LayoutArena::Mark m = a.mark();
  a.alloc<char>(50);
  const size_t peak = a.highWater();
  TEST_ASSERT_TRUE(peak >= 50);

  a.rewind(m);
  TEST_ASSERT_EQUAL_size_t(peak, a.highWater());  // peak retained across rewind
  a.alloc<char>(10);
  TEST_ASSERT_EQUAL_size_t(peak, a.highWater());  // 10 < 50, peak unchanged
}

// The headline RFC #164 invariant: with mark/rewind per paragraph, the peak is
// bounded by ONE paragraph regardless of total word count — the property the
// legacy unbounded ParsedText path could never assert.
void test_arena_bounded_peak_invariant() {
  LayoutArena a = LayoutArena::create(512);
  for (int para = 0; para < 50; ++para) {  // 50 paragraphs x 20 words = 1000 words
    const LayoutArena::Mark m = a.mark();
    bool allFit = true;
    for (int w = 0; w < 20; ++w) {
      if (!a.intern("word", 4).valid()) allFit = false;  // 5 bytes each
    }
    TEST_ASSERT_TRUE(allFit);  // every paragraph fits because we rewind between them
    a.rewind(m);
  }
  // Peak is ~one paragraph (20 * 5 = 100 bytes), nowhere near the cumulative
  // 1000 * 5 = 5000 bytes an unbounded accumulator would have needed.
  TEST_ASSERT_TRUE(a.highWater() >= 100);
  TEST_ASSERT_TRUE(a.highWater() < 256);
}

// ── Shared reader ink-centering kernel (Epub + Txt) ──────────────────────────

// Centered: ink box of height 100 in a 300px viewport, ink starting at y=0 →
// (300-100)/2 - 0 = 100px down.
void test_ink_center_basic_centered() { TEST_ASSERT_EQUAL_INT(100, inkCenterOffset(0, 100, 300)); }

// The inkTop offset is subtracted: the same 100px ink box whose first-line ink
// starts 20px below the content top shifts up by 20 → 100 - 20 = 80.
void test_ink_center_compensates_ascender_band() { TEST_ASSERT_EQUAL_INT(80, inkCenterOffset(20, 120, 300)); }

// Degenerate: zero or negative ink height → no shift (top-aligned).
void test_ink_center_empty_box_is_top_aligned() {
  TEST_ASSERT_EQUAL_INT(0, inkCenterOffset(50, 50, 300));
  TEST_ASSERT_EQUAL_INT(0, inkCenterOffset(80, 40, 300));
}

// Degenerate: ink taller than the viewport → no shift (avoid pushing content
// off-screen), matching both readers' pre-extraction guards.
void test_ink_center_overflowing_box_is_top_aligned() { TEST_ASSERT_EQUAL_INT(0, inkCenterOffset(0, 400, 300)); }

// Exactly viewport-height ink → in range, offset collapses to -inkTop.
void test_ink_center_exact_fit() {
  TEST_ASSERT_EQUAL_INT(0, inkCenterOffset(0, 300, 300));
  TEST_ASSERT_EQUAL_INT(-10, inkCenterOffset(10, 310, 300));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ink_center_basic_centered);
  RUN_TEST(test_ink_center_compensates_ascender_band);
  RUN_TEST(test_ink_center_empty_box_is_top_aligned);
  RUN_TEST(test_ink_center_overflowing_box_is_top_aligned);
  RUN_TEST(test_ink_center_exact_fit);
  RUN_TEST(test_degrade_full_keeps_everything);
  RUN_TEST(test_degrade_levels_shed_in_order);
  RUN_TEST(test_degrade_is_monotone);
  RUN_TEST(test_prewarm_mask_intersects_used_styles);
  RUN_TEST(test_layout_level_bands);
  RUN_TEST(test_render_level_bands);
  RUN_TEST(test_layout_level_is_monotone_in_heap);
  RUN_TEST(test_arena_create_ok_and_zero);
  RUN_TEST(test_arena_alloc_aligns_and_overflows);
  RUN_TEST(test_arena_intern_roundtrip_and_overflow);
  RUN_TEST(test_arena_alloc_and_intern_collide_cleanly);
  RUN_TEST(test_arena_mark_rewind_reclaims);
  RUN_TEST(test_arena_highwater_not_lowered_by_rewind);
  RUN_TEST(test_arena_bounded_peak_invariant);
  return UNITY_END();
}
