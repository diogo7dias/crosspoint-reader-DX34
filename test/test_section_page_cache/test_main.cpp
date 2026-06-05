/**
 * Host-side tests for reader::SectionPageCache<PageT>. Templated on PageT
 * so host tests use a stub FakePage without pulling in HalStorage / Epub.
 * Runs via:  pio test -e test_host -f test_section_page_cache
 */
#include <unity.h>

#include <memory>
#include <set>
#include <utility>

#include "HeapGuard.h"  // crosspoint::heap::setLargestFreeBlockOverride (host seam)
#include "activities/reader/SectionPageCache.h"

using crosspoint::reader::PendingNav;
using crosspoint::reader::SectionPageCache;

namespace {
// Minimal stand-in for ::Page. Stores the page index it was loaded for so
// tests can verify which instance landed in which slot.
struct FakePage {
  int marker = -1;
  explicit FakePage(int m) : marker(m) {}
};

using Cache = SectionPageCache<FakePage>;
using Ptr = std::shared_ptr<FakePage>;

Ptr makePage(int marker) { return std::make_shared<FakePage>(marker); }

// Loader factory — returns a loader that makes a FakePage with marker=page
// and records invocations.
struct LoaderRec {
  std::set<int> loaded;
  Cache::PageLoader fn() {
    return [this](int p) -> Ptr {
      loaded.insert(p);
      return makePage(p);
    };
  }
};
}  // namespace

void setUp() { crosspoint::heap::clearLargestFreeBlockOverride(); }
void tearDown() { crosspoint::heap::clearLargestFreeBlockOverride(); }

// ---- Lifecycle ----
void test_attach_sets_spine_clears_entries() {
  Cache c;
  c.insert(5, makePage(5));
  c.attach(2);
  TEST_ASSERT_EQUAL_INT(2, c.spineIndex());
  TEST_ASSERT_EQUAL_INT(0, c.entryCount());
  TEST_ASSERT_FALSE(c.contains(5));
}

void test_detach_resets_spine_and_entries() {
  Cache c;
  c.attach(3);
  c.insert(1, makePage(1));
  c.detach();
  TEST_ASSERT_EQUAL_INT(-1, c.spineIndex());
  TEST_ASSERT_EQUAL_INT(0, c.entryCount());
}

void test_clear_keeps_spine() {
  Cache c;
  c.attach(9);
  c.insert(1, makePage(1));
  c.clear();
  TEST_ASSERT_EQUAL_INT(9, c.spineIndex());
  TEST_ASSERT_EQUAL_INT(0, c.entryCount());
}

// ---- Lookup ----
void test_get_requires_matching_spine() {
  Cache c;
  c.attach(2);
  c.insert(5, makePage(5));
  TEST_ASSERT_NOT_NULL(c.get(5, 2).get());
  TEST_ASSERT_NULL(c.get(5, 3).get());  // wrong spine
}

void test_get_missing_page_returns_null() {
  Cache c;
  c.attach(0);
  c.insert(10, makePage(10));
  TEST_ASSERT_NULL(c.get(11, 0).get());
}

// ---- Insert ----
void test_insert_rotates_lru_on_new_page() {
  Cache c;
  c.attach(0);
  c.insert(1, makePage(1));
  c.insert(2, makePage(2));
  c.insert(3, makePage(3));
  // All 3 slots filled: [1, 2, 3]
  TEST_ASSERT_EQUAL_INT(3, c.entryCount());
  TEST_ASSERT_TRUE(c.contains(1));

  c.insert(4, makePage(4));  // rotation evicts oldest (1)
  TEST_ASSERT_FALSE(c.contains(1));
  TEST_ASSERT_TRUE(c.contains(2));
  TEST_ASSERT_TRUE(c.contains(3));
  TEST_ASSERT_TRUE(c.contains(4));
  TEST_ASSERT_EQUAL_INT(3, c.entryCount());
}

void test_insert_existing_page_refreshes_in_place() {
  Cache c;
  c.attach(0);
  c.insert(1, makePage(1));
  c.insert(2, makePage(2));
  c.insert(3, makePage(3));

  // Re-insert page 1 with a new marker; must NOT rotate out page 2 or 3.
  c.insert(1, makePage(100));
  TEST_ASSERT_TRUE(c.contains(1));
  TEST_ASSERT_TRUE(c.contains(2));
  TEST_ASSERT_TRUE(c.contains(3));
  TEST_ASSERT_EQUAL_INT(100, c.get(1, 0)->marker);
}

void test_insert_null_is_noop() {
  Cache c;
  c.attach(0);
  c.insert(5, Ptr{});
  TEST_ASSERT_EQUAL_INT(0, c.entryCount());
}

// ---- refreshWindow ----
void test_refresh_window_middle_page() {
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(/*center=*/5, /*current=*/makePage(5), /*pc=*/10, rec.fn());

  TEST_ASSERT_EQUAL_INT(3, c.entryCount());
  TEST_ASSERT_TRUE(c.contains(4));
  TEST_ASSERT_TRUE(c.contains(5));
  TEST_ASSERT_TRUE(c.contains(6));
  // Loader invoked only for neighbours, not the center.
  TEST_ASSERT_EQUAL_INT(2u, rec.loaded.size());
  TEST_ASSERT_TRUE(rec.loaded.count(4) == 1);
  TEST_ASSERT_TRUE(rec.loaded.count(6) == 1);
  TEST_ASSERT_TRUE(rec.loaded.count(5) == 0);
  // Center page is the one we injected.
  TEST_ASSERT_EQUAL_INT(5, c.get(5, 0)->marker);
}

void test_refresh_window_first_page_leaves_prev_empty() {
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(0, makePage(0), 10, rec.fn());

  TEST_ASSERT_EQUAL_INT(2, c.entryCount());  // only 0 and 1
  TEST_ASSERT_TRUE(c.contains(0));
  TEST_ASSERT_TRUE(c.contains(1));
  TEST_ASSERT_FALSE(c.contains(-1));
  TEST_ASSERT_EQUAL_INT(1u, rec.loaded.size());
  TEST_ASSERT_TRUE(rec.loaded.count(1) == 1);
}

void test_refresh_window_last_page_leaves_next_empty() {
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(9, makePage(9), 10, rec.fn());

  TEST_ASSERT_EQUAL_INT(2, c.entryCount());
  TEST_ASSERT_TRUE(c.contains(8));
  TEST_ASSERT_TRUE(c.contains(9));
  TEST_ASSERT_FALSE(c.contains(10));
  TEST_ASSERT_EQUAL_INT(1u, rec.loaded.size());
  TEST_ASSERT_TRUE(rec.loaded.count(8) == 1);
}

void test_refresh_window_out_of_bounds_clears() {
  Cache c;
  c.attach(0);
  c.insert(1, makePage(1));
  LoaderRec rec;

  c.refreshWindow(-1, makePage(-1), 10, rec.fn());
  TEST_ASSERT_EQUAL_INT(0, c.entryCount());
  TEST_ASSERT_EQUAL_INT(0u, rec.loaded.size());

  c.insert(5, makePage(5));
  c.refreshWindow(20, makePage(20), 10, rec.fn());
  TEST_ASSERT_EQUAL_INT(0, c.entryCount());
}

void test_refresh_window_reuses_cached_neighbours() {
  Cache c;
  c.attach(0);
  LoaderRec rec;

  // Seed cache with 5's neighbours first.
  c.refreshWindow(5, makePage(5), 10, rec.fn());
  rec.loaded.clear();

  // Slide to 6 — neighbour 5 is already cached; loader shouldn't re-fetch it.
  // Only page 7 (new neighbour) needs loading. Page 4 falls out.
  c.refreshWindow(6, makePage(6), 10, rec.fn());
  TEST_ASSERT_TRUE(c.contains(5));
  TEST_ASSERT_TRUE(c.contains(6));
  TEST_ASSERT_TRUE(c.contains(7));
  TEST_ASSERT_FALSE(c.contains(4));
  TEST_ASSERT_EQUAL_INT(1u, rec.loaded.size());
  TEST_ASSERT_TRUE(rec.loaded.count(7) == 1);
}

void test_refresh_window_single_page_section() {
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(0, makePage(0), 1, rec.fn());
  TEST_ASSERT_EQUAL_INT(1, c.entryCount());
  TEST_ASSERT_TRUE(c.contains(0));
  TEST_ASSERT_EQUAL_INT(0u, rec.loaded.size());
}

// ---- refreshWindow heap-gated prefetch (RFC reading-speed Stage 1a) ----

// Moderate fragmentation (18 KB <= largest < 30 KB): the FORWARD slot still
// prefetches (cheaper PrefetchNextPage gate), the BACKWARD slot is skipped
// (full PrefetchNeighborPages gate closed). This is the fix for the on-turn
// SD-load stalls during forward reading.
void test_refresh_window_forward_prefetch_under_moderate_pressure() {
  crosspoint::heap::setLargestFreeBlockOverride(20 * 1024);
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(/*center=*/5, /*current=*/makePage(5), /*pc=*/10, rec.fn());

  TEST_ASSERT_TRUE(c.contains(5));   // center always cached
  TEST_ASSERT_TRUE(c.contains(6));   // forward prefetched (18 KB gate open)
  TEST_ASSERT_FALSE(c.contains(4));  // backward skipped (30 KB gate closed)
  TEST_ASSERT_EQUAL_INT(2, c.entryCount());
  TEST_ASSERT_EQUAL_INT(1u, rec.loaded.size());
  TEST_ASSERT_TRUE(rec.loaded.count(6) == 1);
  TEST_ASSERT_TRUE(rec.loaded.count(4) == 0);
}

// Severe fragmentation (< 18 KB largest): both neighbours skipped, only the
// current page cached — the original tight-heap behaviour preserved.
void test_refresh_window_no_prefetch_under_severe_pressure() {
  crosspoint::heap::setLargestFreeBlockOverride(10 * 1024);
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(5, makePage(5), 10, rec.fn());

  TEST_ASSERT_TRUE(c.contains(5));
  TEST_ASSERT_FALSE(c.contains(4));
  TEST_ASSERT_FALSE(c.contains(6));
  TEST_ASSERT_EQUAL_INT(1, c.entryCount());
  TEST_ASSERT_EQUAL_INT(0u, rec.loaded.size());
}

// Healthy heap (>= 30 KB): both neighbours prefetched (unchanged behaviour).
void test_refresh_window_full_prefetch_when_healthy() {
  crosspoint::heap::setLargestFreeBlockOverride(64 * 1024);
  Cache c;
  c.attach(0);
  LoaderRec rec;

  c.refreshWindow(5, makePage(5), 10, rec.fn());

  TEST_ASSERT_TRUE(c.contains(4));
  TEST_ASSERT_TRUE(c.contains(5));
  TEST_ASSERT_TRUE(c.contains(6));
  TEST_ASSERT_EQUAL_INT(3, c.entryCount());
  TEST_ASSERT_EQUAL_INT(2u, rec.loaded.size());
}

// ---- Pending-nav bundle ----
void test_pending_nav_empty_by_default() {
  Cache c;
  TEST_ASSERT_FALSE(c.hasPending());
  TEST_ASSERT_TRUE(c.pending().empty());
}

void test_pending_nav_percent_jump() {
  Cache c;
  PendingNav n;
  n.hasPercentJump = true;
  n.spineProgress = 0.42f;
  c.setPending(n);

  TEST_ASSERT_TRUE(c.hasPending());
  TEST_ASSERT_TRUE(c.pending().hasPercentJump);
  TEST_ASSERT_EQUAL_FLOAT(0.42f, c.pending().spineProgress);
  TEST_ASSERT_FALSE(c.pending().hasAnchor);

  c.clearPending();
  TEST_ASSERT_FALSE(c.hasPending());
  TEST_ASSERT_FALSE(c.pending().hasPercentJump);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.pending().spineProgress);
}

void test_pending_nav_anchor() {
  Cache c;
  PendingNav n;
  n.hasAnchor = true;
  n.anchor = "chap3";
  c.setPending(std::move(n));

  TEST_ASSERT_TRUE(c.hasPending());
  TEST_ASSERT_EQUAL_STRING("chap3", c.pending().anchor.c_str());

  c.clearPending();
  TEST_ASSERT_TRUE(c.pending().anchor.empty());
  TEST_ASSERT_FALSE(c.pending().hasAnchor);
}

// Attach/detach must NOT touch pending-nav — it describes navigation
// intent for the spine we're about to attach to.
void test_pending_nav_survives_attach() {
  Cache c;
  PendingNav n;
  n.hasPercentJump = true;
  n.spineProgress = 0.7f;
  c.setPending(n);

  c.attach(5);
  TEST_ASSERT_TRUE(c.hasPending());
  TEST_ASSERT_EQUAL_FLOAT(0.7f, c.pending().spineProgress);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_attach_sets_spine_clears_entries);
  RUN_TEST(test_detach_resets_spine_and_entries);
  RUN_TEST(test_clear_keeps_spine);
  RUN_TEST(test_get_requires_matching_spine);
  RUN_TEST(test_get_missing_page_returns_null);
  RUN_TEST(test_insert_rotates_lru_on_new_page);
  RUN_TEST(test_insert_existing_page_refreshes_in_place);
  RUN_TEST(test_insert_null_is_noop);
  RUN_TEST(test_refresh_window_middle_page);
  RUN_TEST(test_refresh_window_first_page_leaves_prev_empty);
  RUN_TEST(test_refresh_window_last_page_leaves_next_empty);
  RUN_TEST(test_refresh_window_out_of_bounds_clears);
  RUN_TEST(test_refresh_window_reuses_cached_neighbours);
  RUN_TEST(test_refresh_window_single_page_section);
  RUN_TEST(test_refresh_window_forward_prefetch_under_moderate_pressure);
  RUN_TEST(test_refresh_window_no_prefetch_under_severe_pressure);
  RUN_TEST(test_refresh_window_full_prefetch_when_healthy);
  RUN_TEST(test_pending_nav_empty_by_default);
  RUN_TEST(test_pending_nav_percent_jump);
  RUN_TEST(test_pending_nav_anchor);
  RUN_TEST(test_pending_nav_survives_attach);
  return UNITY_END();
}
