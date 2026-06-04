// Host tests for ReaderSession (RFC #171): the onEnter skeleton + progress
// observe/debounce/flush orchestration, driven through in-memory port fakes.
//   pio test -e test_host -f test_reader_session
#include <unity.h>

#include <string>
#include <vector>

#include "activities/reader/ReaderSession.h"

using crosspoint::reader::ReaderHooks;
using crosspoint::reader::ReaderPosition;
using crosspoint::reader::ReaderSession;

namespace {

struct FakeSink : crosspoint::reader::IProgressSink {
  std::vector<ReaderPosition> writes;
  bool write(const ReaderPosition& p) override {
    writes.push_back(p);
    return true;
  }
};

struct FakeEnv : crosspoint::reader::IReaderEnvPort {
  bool fullRefresh = true;
  bool bold = false;
  std::string moveResult;
  int refreshCalls = 0, registerCalls = 0;
  std::string lastTitle, lastAuthor, lastThumb, lastRegisterPath;
  bool shouldFullRefreshOnEnter(const std::string&) override {
    ++refreshCalls;
    return fullRefresh;
  }
  bool boldSwap(const std::string&) const override { return bold; }
  void registerOpened(const std::string& path, const std::string& title, const std::string& author,
                      const std::string& thumb) override {
    ++registerCalls;
    lastRegisterPath = path;
    lastTitle = title;
    lastAuthor = author;
    lastThumb = thumb;
  }
  std::string moveBookToRecents(const std::string&) override { return moveResult; }
};

struct FakeDisplay : crosspoint::reader::IReaderDisplayPort {
  std::vector<bool> refreshes;  // true = full
  int orientationApplies = 0;
  std::vector<bool> boldCalls;
  void requestRefresh(bool full) override { refreshes.push_back(full); }
  void applyOrientationFromSettings() override { ++orientationApplies; }
  void setBoldSwap(bool enabled) override { boldCalls.push_back(enabled); }
};

// Build a session over a fixed position cell the test can move.
struct Harness {
  FakeSink sink;
  FakeEnv env;
  FakeDisplay display;
  ReaderPosition pos{0, 0, 1};
  std::string path = "/books/a.txt";
  std::vector<std::string> order;  // records hook fire order
  ReaderSession session;

  Harness()
      : session({sink, env, display},
                ReaderHooks{
                    [this] { return path; },
                    [this] { return pos; },
                    [this] { order.push_back("beforeRefresh"); },
                    [this] { order.push_back("afterOrientation"); },
                    [this] { order.push_back("afterRegister"); },
                    [this](std::string& t, std::string& a, std::string& th) {
                      t = "Title";
                      a = "Author";
                      th = "/t.bmp";
                    }}) {}
};

}  // namespace

void setUp() {}
void tearDown() {}

void test_enter_full_refresh_registers_and_seeds() {
  Harness h;
  h.env.fullRefresh = true;
  h.env.bold = true;
  const std::string moved = h.session.enter({0, 5, 1});

  TEST_ASSERT_EQUAL(1, h.display.refreshes.size());
  TEST_ASSERT_TRUE(h.display.refreshes.back());  // full
  TEST_ASSERT_EQUAL(1, h.display.orientationApplies);
  TEST_ASSERT_TRUE(h.display.boldCalls.back());  // bold-swap on from env
  TEST_ASSERT_EQUAL(1, h.env.registerCalls);
  TEST_ASSERT_EQUAL_STRING("Title", h.env.lastTitle.c_str());
  TEST_ASSERT_EQUAL_STRING("", moved.c_str());            // not relocated
  TEST_ASSERT_EQUAL(0, h.sink.writes.size());             // seed performs no write
  TEST_ASSERT_EQUAL(5, h.session.progress().lastSaved().page);  // seeded
}

void test_enter_half_refresh_and_relocation_returns_new_path() {
  Harness h;
  h.env.fullRefresh = false;
  h.env.moveResult = "/recents/a.txt";
  const std::string moved = h.session.enter({0, 0, 1});
  TEST_ASSERT_FALSE(h.display.refreshes.back());  // half
  TEST_ASSERT_EQUAL_STRING("/recents/a.txt", moved.c_str());
}

void test_enter_fires_hooks_in_order() {
  Harness h;
  h.session.enter({0, 0, 1});
  // beforeRefresh precedes orientation precedes register.
  TEST_ASSERT_EQUAL(3, h.order.size());
  TEST_ASSERT_EQUAL_STRING("beforeRefresh", h.order[0].c_str());
  TEST_ASSERT_EQUAL_STRING("afterOrientation", h.order[1].c_str());
  TEST_ASSERT_EQUAL_STRING("afterRegister", h.order[2].c_str());
}

void test_tick_debounces_then_writes() {
  Harness h;
  h.session.enter({0, 0, 1});
  h.pos = {0, 1, 1};
  h.session.tick(1000, false);          // observe page 1, anchor debounce
  TEST_ASSERT_EQUAL(0, h.sink.writes.size());
  h.session.tick(1500, false);          // < 800ms since change? change was at 1000, now 1500 -> 500ms
  TEST_ASSERT_EQUAL(0, h.sink.writes.size());
  h.session.tick(1900, false);          // 900ms elapsed -> write
  TEST_ASSERT_EQUAL(1, h.sink.writes.size());
  TEST_ASSERT_EQUAL(1, h.sink.writes.back().page);
}

void test_tick_force_writes_immediately() {
  Harness h;
  h.session.enter({0, 0, 1});
  h.pos = {0, 7, 1};
  h.session.tick(100, true);            // force ignores debounce
  TEST_ASSERT_EQUAL(1, h.sink.writes.size());
  TEST_ASSERT_EQUAL(7, h.sink.writes.back().page);
}

void test_tick_idempotent_when_unchanged() {
  Harness h;
  h.session.enter({0, 3, 1});
  h.pos = {0, 3, 1};                    // same as seed
  h.session.tick(5000, true);          // nothing dirty -> no write
  TEST_ASSERT_EQUAL(0, h.sink.writes.size());
}

void test_exit_force_flushes_and_clears_boldswap() {
  Harness h;
  h.session.enter({0, 0, 1});
  h.pos = {0, 9, 1};
  h.session.exit(2000);
  TEST_ASSERT_EQUAL(1, h.sink.writes.size());
  TEST_ASSERT_EQUAL(9, h.sink.writes.back().page);
  TEST_ASSERT_FALSE(h.display.boldCalls.back());  // bold-swap disabled on exit
}

void test_enter_skips_orientation_when_opted_out() {
  // XTC path: applyOrientationOnEnter=false — pre-rendered, never orients on open.
  FakeSink sink;
  FakeEnv env;
  FakeDisplay display;
  ReaderSession s({sink, env, display},
                  ReaderHooks{[] { return std::string("/b.xtc"); }, [] { return ReaderPosition{0, 0, 1}; }},
                  crosspoint::reader::ReaderProgressTracker::kDefaultDebounceMs, /*applyOrientationOnEnter=*/false);
  s.enter({0, 0, 1});
  TEST_ASSERT_EQUAL(0, display.orientationApplies);  // not applied
  TEST_ASSERT_EQUAL(1, display.refreshes.size());    // refresh still happens
}

void test_resetto_flushes_then_reseeds() {
  Harness h;
  h.session.enter({0, 0, 1});
  h.pos = {0, 2, 1};
  h.session.tick(1000, false);         // dirty at page 2, not yet flushed
  h.session.resetTo({0, 50, 1}, 1100); // KOReader sync: flush pending then reseed
  // pending page-2 write flushed, then reseed to 50, dirty cleared.
  TEST_ASSERT_EQUAL(2, h.sink.writes.back().page);
  TEST_ASSERT_EQUAL(50, h.session.progress().lastSaved().page);
  h.pos = {0, 50, 1};
  h.session.tick(9999, true);          // reseeded position is not dirty -> no extra write
  TEST_ASSERT_EQUAL(1, h.sink.writes.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_enter_full_refresh_registers_and_seeds);
  RUN_TEST(test_enter_half_refresh_and_relocation_returns_new_path);
  RUN_TEST(test_enter_fires_hooks_in_order);
  RUN_TEST(test_tick_debounces_then_writes);
  RUN_TEST(test_tick_force_writes_immediately);
  RUN_TEST(test_tick_idempotent_when_unchanged);
  RUN_TEST(test_exit_force_flushes_and_clears_boldswap);
  RUN_TEST(test_enter_skips_orientation_when_opted_out);
  RUN_TEST(test_resetto_flushes_then_reseeds);
  return UNITY_END();
}
