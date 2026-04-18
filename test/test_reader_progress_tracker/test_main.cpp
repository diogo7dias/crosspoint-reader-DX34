/**
 * Host-side tests for reader::ReaderProgressTracker. Pure state-machine +
 * debounce tests; no ESP32 / SD / Hal. Runs via:
 *   pio test -e test_host -f test_reader_progress_tracker
 */
#include <unity.h>

#include <vector>

#include "activities/reader/ReaderProgressTracker.h"

using crosspoint::reader::IProgressSink;
using crosspoint::reader::ReaderPosition;
using crosspoint::reader::ReaderProgressTracker;

namespace {

// Records every successful write. Fails on demand to exercise retry path.
class FakeSink : public IProgressSink {
 public:
  std::vector<ReaderPosition> writes;
  bool nextWriteFails = false;

  bool write(const ReaderPosition& p) override {
    if (nextWriteFails) {
      nextWriteFails = false;
      return false;
    }
    writes.push_back(p);
    return true;
  }
};

ReaderPosition P(int32_t s, int32_t pg, int32_t pc) { return ReaderPosition{s, pg, pc}; }

}  // namespace

void setUp() {}
void tearDown() {}

// Seed snaps observed+saved, clears dirty, no write.
void test_seed_no_write() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 800);
  t.seed(P(3, 10, 20));
  TEST_ASSERT_EQUAL_INT(0u, sink.writes.size());
  TEST_ASSERT_FALSE(t.dirty());
  TEST_ASSERT_EQUAL_INT(3, t.lastSaved().spineIndex);
  TEST_ASSERT_EQUAL_INT(10, t.lastSaved().page);
}

// Observing same position never marks dirty or writes.
void test_observe_same_position_noop() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 800);
  t.seed(P(0, 0, 1));
  t.observe(P(0, 0, 1), 100);
  t.observe(P(0, 0, 1), 500);
  TEST_ASSERT_FALSE(t.dirty());
  TEST_ASSERT_FALSE(t.flush(10'000, false));
  TEST_ASSERT_EQUAL_INT(0u, sink.writes.size());
}

// Rapid observes coalesce into one write under debounce.
void test_debounce_coalesces() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 1000);
  t.seed(P(0, 0, 10));

  t.observe(P(0, 1, 10), 100);
  t.observe(P(0, 2, 10), 200);
  t.observe(P(0, 3, 10), 300);
  TEST_ASSERT_TRUE(t.dirty());

  // Not enough time passed since lastChangeMs (300). 900 < 300+1000.
  TEST_ASSERT_FALSE(t.flush(900, false));
  TEST_ASSERT_EQUAL_INT(0u, sink.writes.size());

  // Debounce elapsed.
  TEST_ASSERT_TRUE(t.flush(1500, false));
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());
  TEST_ASSERT_EQUAL_INT(3, sink.writes[0].page);
  TEST_ASSERT_FALSE(t.dirty());
}

// Force flush bypasses debounce.
void test_force_flush_ignores_debounce() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 5000);
  t.seed(P(0, 0, 1));

  t.observe(P(1, 5, 10), 100);
  TEST_ASSERT_TRUE(t.flush(100, /*force=*/true));
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());
  TEST_ASSERT_EQUAL_INT(1, sink.writes[0].spineIndex);
  TEST_ASSERT_EQUAL_INT(5, sink.writes[0].page);
}

// Clean tracker (never dirtied) does not write on force flush.
void test_flush_clean_is_noop() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 800);
  t.seed(P(2, 4, 8));
  TEST_ASSERT_FALSE(t.flush(10'000, true));
  TEST_ASSERT_EQUAL_INT(0u, sink.writes.size());
}

// After flush, re-observing the same rendered position must NOT re-dirty.
void test_no_redirty_after_flush() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 500);
  t.seed(P(0, 0, 10));

  t.observe(P(0, 5, 10), 100);
  TEST_ASSERT_TRUE(t.flush(1000, true));
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());

  t.observe(P(0, 5, 10), 2000);
  TEST_ASSERT_FALSE(t.dirty());
  TEST_ASSERT_FALSE(t.flush(3000, true));
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());
}

// Observing back-and-forth between current render and lastSaved clears dirty:
// if rendered matches lastSaved, tracker is not dirty.
void test_observe_matching_saved_not_dirty() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 500);
  t.seed(P(0, 5, 10));

  t.observe(P(0, 6, 10), 100);    // dirty
  TEST_ASSERT_TRUE(t.dirty());
  t.observe(P(0, 5, 10), 200);    // matches saved — dirty stays false after
  TEST_ASSERT_FALSE(t.dirty());
  TEST_ASSERT_FALSE(t.flush(5000, true));
  TEST_ASSERT_EQUAL_INT(0u, sink.writes.size());
}

// Failed write leaves tracker dirty so next flush retries.
void test_failed_write_retries() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 100);
  t.seed(P(0, 0, 1));

  t.observe(P(0, 1, 1), 0);
  sink.nextWriteFails = true;
  TEST_ASSERT_FALSE(t.flush(1000, true));   // write failed
  TEST_ASSERT_TRUE(t.dirty());              // still dirty
  TEST_ASSERT_EQUAL_INT(0u, sink.writes.size());

  TEST_ASSERT_TRUE(t.flush(2000, true));    // retry succeeds
  TEST_ASSERT_FALSE(t.dirty());
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());
  TEST_ASSERT_EQUAL_INT(1, sink.writes[0].page);
}

// snapshotForReset flushes any pending write THEN seeds new position.
// Simulates KOReaderSync pulling a new position while a debounced turn is
// pending.
void test_snapshot_for_reset_flushes_then_seeds() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 5000);
  t.seed(P(0, 10, 100));

  t.observe(P(0, 11, 100), 100);   // local page turn, debounced
  TEST_ASSERT_TRUE(t.dirty());

  // KOReader says jump to chapter 2, page 0.
  t.snapshotForReset(P(2, 0, 0), 200);

  // Pending local turn was persisted before swap — zero data loss.
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());
  TEST_ASSERT_EQUAL_INT(11, sink.writes[0].page);

  // Tracker now reports the KOReader position as both observed and saved.
  TEST_ASSERT_FALSE(t.dirty());
  TEST_ASSERT_EQUAL_INT(2, t.lastSaved().spineIndex);
  TEST_ASSERT_EQUAL_INT(0, t.lastSaved().page);

  // Subsequent observe of same post-reset position stays clean.
  t.observe(P(2, 0, 0), 300);
  TEST_ASSERT_FALSE(t.dirty());
}

// After seed, setting new debounce takes effect on next observe.
void test_set_debounce_changes_window() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 5000);
  t.seed(P(0, 0, 1));

  t.setDebounceMs(100);
  t.observe(P(0, 1, 1), 0);
  TEST_ASSERT_FALSE(t.flush(50, false));
  TEST_ASSERT_TRUE(t.flush(200, false));
  TEST_ASSERT_EQUAL_INT(1u, sink.writes.size());
}

// flushCount tracks successful writes.
void test_flush_count() {
  FakeSink sink;
  ReaderProgressTracker t(sink, 100);
  t.seed(P(0, 0, 1));

  for (int i = 1; i <= 3; ++i) {
    t.observe(P(0, i, 1), i * 1000);
    TEST_ASSERT_TRUE(t.flush(i * 1000 + 200, false));
  }
  TEST_ASSERT_EQUAL_INT(3u, t.flushCount());
  TEST_ASSERT_EQUAL_INT(3u, sink.writes.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_seed_no_write);
  RUN_TEST(test_observe_same_position_noop);
  RUN_TEST(test_debounce_coalesces);
  RUN_TEST(test_force_flush_ignores_debounce);
  RUN_TEST(test_flush_clean_is_noop);
  RUN_TEST(test_no_redirty_after_flush);
  RUN_TEST(test_observe_matching_saved_not_dirty);
  RUN_TEST(test_failed_write_retries);
  RUN_TEST(test_snapshot_for_reset_flushes_then_seeds);
  RUN_TEST(test_set_debounce_changes_window);
  RUN_TEST(test_flush_count);
  return UNITY_END();
}
