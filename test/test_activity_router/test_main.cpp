// Host-side tests for lifecycle::ActivityRouter (issue #25).
//
// Runs under PlatformIO [env:test_host] — pure std::, no Arduino/ESP32.
// Activity is stubbed via ActivityStubForHostTest.h (pulled in by
// ActivityRouter.cpp when -DUNIT_TEST_HOST=1).
//
// Invoke:  pio test -e test_host

#include <unity.h>

#include <string>
#include <vector>

#include "lifecycle/ActivityRouter.h"
#include "lifecycle/ActivityStubForHostTest.h"
#include "lifecycle/Nav.h"

using lifecycle::ActivityRouter;
using lifecycle::Nav;
using lifecycle::policyFor;
using lifecycle::RouteId;

namespace {

// --- Recording globals. Router Deps take function pointers, so recorders must
// be statics with free-function thunks. Cleared in setUp().
std::vector<std::string> g_persistCalls;
int g_trimCalls = 0;
std::vector<bool> g_beforeSleep;
int g_afterSleep = 0;
int g_enterSleep = 0;
std::vector<std::string> g_factoryCalls;  // "Route:payload"

bool thunkPersist(const char* ctx) {
  g_persistCalls.emplace_back(ctx ? ctx : "");
  return true;
}
void thunkTrim() { g_trimCalls++; }
void thunkBefore(bool fromReader) { g_beforeSleep.push_back(fromReader); }
void thunkAfter() { g_afterSleep++; }

const char* routeName(RouteId r) {
  switch (r) {
    case RouteId::Home:
      return "Home";
    case RouteId::Reader:
      return "Reader";
    case RouteId::MyLibrary:
      return "MyLibrary";
    case RouteId::MyLibraryAt:
      return "MyLibraryAt";
    case RouteId::RecentBooks:
      return "RecentBooks";
    case RouteId::Settings:
      return "Settings";
    case RouteId::FileTransfer:
      return "FileTransfer";
    case RouteId::Browser:
      return "Browser";
  }
  return "?";
}

void registerRecordingFactories() {
  auto& r = ActivityRouter::instance();
  const RouteId all[] = {RouteId::Home,        RouteId::Reader,   RouteId::MyLibrary,    RouteId::MyLibraryAt,
                         RouteId::RecentBooks, RouteId::Settings, RouteId::FileTransfer, RouteId::Browser};
  for (RouteId id : all) {
    const char* name = routeName(id);
    r.setRouteFactory(id, [name](const std::string& p) { g_factoryCalls.push_back(std::string(name) + ":" + p); });
  }
}

}  // namespace

void setUp() {
  ActivityRouter::resetForTest();
  g_persistCalls.clear();
  g_trimCalls = 0;
  g_beforeSleep.clear();
  g_afterSleep = 0;
  g_enterSleep = 0;
  g_factoryCalls.clear();
}

void tearDown() {}

// ---- Policy tests ----

void test_policy_exhaustive_for_all_routes() {
  // Exercises the switch in policyFor for every RouteId. If a new enum value
  // is added without updating the switch, the compile warning + unreachable
  // sentinel would be exposed here. Runtime check also confirms expected rows.
  TEST_ASSERT_TRUE(policyFor(RouteId::Home).persistBefore);
  TEST_ASSERT_TRUE(policyFor(RouteId::Home).trimSleepFolder);
  TEST_ASSERT_EQUAL_STRING("go home", policyFor(RouteId::Home).persistCtx);

  TEST_ASSERT_TRUE(policyFor(RouteId::MyLibrary).persistBefore);
  TEST_ASSERT_TRUE(policyFor(RouteId::MyLibraryAt).persistBefore);
  TEST_ASSERT_TRUE(policyFor(RouteId::RecentBooks).persistBefore);

  TEST_ASSERT_FALSE(policyFor(RouteId::Reader).persistBefore);
  TEST_ASSERT_FALSE(policyFor(RouteId::Settings).persistBefore);
  TEST_ASSERT_FALSE(policyFor(RouteId::FileTransfer).persistBefore);
  TEST_ASSERT_FALSE(policyFor(RouteId::Browser).persistBefore);
}

// ---- Router dispatch tests ----

void test_request_home_persists_before_factory_runs() {
  registerRecordingFactories();
  ActivityRouter::Deps d{};
  d.persistAppState = &thunkPersist;
  d.trimSleepFolderIfDirty = &thunkTrim;
  ActivityRouter::setDepsForTest(d);

  ActivityRouter::instance().request({RouteId::Home, ""});
  ActivityRouter::instance().applyIfPending();

  TEST_ASSERT_EQUAL(1, g_trimCalls);
  TEST_ASSERT_EQUAL_size_t(1, g_persistCalls.size());
  TEST_ASSERT_EQUAL_STRING("go home", g_persistCalls[0].c_str());
  TEST_ASSERT_EQUAL_size_t(1, g_factoryCalls.size());
  TEST_ASSERT_EQUAL_STRING("Home:", g_factoryCalls[0].c_str());
}

void test_no_persist_routes_skip_persist() {
  registerRecordingFactories();
  ActivityRouter::Deps d{};
  d.persistAppState = &thunkPersist;
  d.trimSleepFolderIfDirty = &thunkTrim;
  ActivityRouter::setDepsForTest(d);

  const RouteId skipRoutes[] = {RouteId::Reader, RouteId::Settings, RouteId::FileTransfer, RouteId::Browser};
  for (RouteId r : skipRoutes) {
    ActivityRouter::instance().request({r, "x"});
    ActivityRouter::instance().applyIfPending();
  }

  TEST_ASSERT_EQUAL_size_t(0, g_persistCalls.size());
  TEST_ASSERT_EQUAL(0, g_trimCalls);
  TEST_ASSERT_EQUAL_size_t(4, g_factoryCalls.size());
}

void test_pending_coalesce_keeps_last_request() {
  registerRecordingFactories();
  ActivityRouter::setDepsForTest(ActivityRouter::Deps{});

  ActivityRouter::instance().request({RouteId::Settings, "first"});
  ActivityRouter::instance().request({RouteId::Browser, "second"});
  ActivityRouter::instance().applyIfPending();

  TEST_ASSERT_EQUAL_size_t(1, g_factoryCalls.size());
  TEST_ASSERT_EQUAL_STRING("Browser:second", g_factoryCalls[0].c_str());
}

void test_apply_if_pending_noop_without_pending() {
  registerRecordingFactories();
  ActivityRouter::setDepsForTest(ActivityRouter::Deps{});

  ActivityRouter::instance().applyIfPending();
  ActivityRouter::instance().applyIfPending();

  TEST_ASSERT_EQUAL_size_t(0, g_factoryCalls.size());
}

void test_begin_dispatches_synchronously_with_payload() {
  registerRecordingFactories();
  ActivityRouter::Deps d{};
  d.persistAppState = &thunkPersist;
  ActivityRouter::setDepsForTest(d);

  ActivityRouter::instance().begin({RouteId::Reader, "/books/foo.epub"});

  // No persist for Reader.
  TEST_ASSERT_EQUAL_size_t(0, g_persistCalls.size());
  TEST_ASSERT_EQUAL_size_t(1, g_factoryCalls.size());
  TEST_ASSERT_EQUAL_STRING("Reader:/books/foo.epub", g_factoryCalls[0].c_str());
}

void test_begin_home_applies_policy_before_factory() {
  registerRecordingFactories();
  ActivityRouter::Deps d{};
  d.persistAppState = &thunkPersist;
  d.trimSleepFolderIfDirty = &thunkTrim;
  ActivityRouter::setDepsForTest(d);

  ActivityRouter::instance().begin({RouteId::Home, ""});

  TEST_ASSERT_EQUAL(1, g_trimCalls);
  TEST_ASSERT_EQUAL_size_t(1, g_persistCalls.size());
  TEST_ASSERT_EQUAL_STRING("go home", g_persistCalls[0].c_str());
  TEST_ASSERT_EQUAL_size_t(1, g_factoryCalls.size());
  TEST_ASSERT_EQUAL_STRING("Home:", g_factoryCalls[0].c_str());
}

void test_unmigrated_route_is_noop() {
  // No factory registered → router silently does nothing (legacy caller path).
  ActivityRouter::setDepsForTest(ActivityRouter::Deps{});
  ActivityRouter::instance().request({RouteId::Home, ""});
  ActivityRouter::instance().applyIfPending();
  TEST_ASSERT_EQUAL_size_t(0, g_factoryCalls.size());
}

// ---- Deep-sleep sequencing ----

// Ordering is captured by recording every hook into a single vector of tags.
std::vector<std::string> g_sleepOrder;

void orderBefore(bool fromReader) { g_sleepOrder.push_back(fromReader ? "before:true" : "before:false"); }
bool orderPersist(const char* ctx) {
  g_sleepOrder.push_back(std::string("persist:") + (ctx ? ctx : ""));
  return true;
}
void orderAfter() { g_sleepOrder.push_back("after"); }

class OrderExitActivity : public Activity {
 public:
  void onExit() override { g_sleepOrder.push_back("exit"); }
};

void setUpSleep() {
  setUp();
  g_sleepOrder.clear();
}

void test_enter_deep_sleep_sequence_from_reader() {
  setUpSleep();
  Activity* slot = new OrderExitActivity();

  ActivityRouter::Deps d{};
  d.currentActivitySlot = &slot;
  d.persistAppState = &orderPersist;
  d.onBeforeDeepSleep = &orderBefore;
  d.onAfterDeepSleep = &orderAfter;
  d.enterSleepActivity = [] { g_sleepOrder.push_back("sleepActivity"); };
  ActivityRouter::setDepsForTest(d);

  ActivityRouter::instance().enterDeepSleep(true);

  TEST_ASSERT_NULL(slot);
  TEST_ASSERT_EQUAL_size_t(5, g_sleepOrder.size());
  TEST_ASSERT_EQUAL_STRING("before:true", g_sleepOrder[0].c_str());
  TEST_ASSERT_EQUAL_STRING("persist:enter deep sleep", g_sleepOrder[1].c_str());
  TEST_ASSERT_EQUAL_STRING("exit", g_sleepOrder[2].c_str());
  TEST_ASSERT_EQUAL_STRING("sleepActivity", g_sleepOrder[3].c_str());
  TEST_ASSERT_EQUAL_STRING("after", g_sleepOrder[4].c_str());
}

void test_enter_deep_sleep_sequence_not_from_reader() {
  setUpSleep();
  Activity* slot = nullptr;  // no active slot — exit hook must be skipped.

  ActivityRouter::Deps d{};
  d.currentActivitySlot = &slot;
  d.persistAppState = &orderPersist;
  d.onBeforeDeepSleep = &orderBefore;
  d.onAfterDeepSleep = &orderAfter;
  d.enterSleepActivity = [] { g_sleepOrder.push_back("sleepActivity"); };
  ActivityRouter::setDepsForTest(d);

  ActivityRouter::instance().enterDeepSleep(false);

  // Exit tag absent because slot was null.
  TEST_ASSERT_EQUAL_size_t(4, g_sleepOrder.size());
  TEST_ASSERT_EQUAL_STRING("before:false", g_sleepOrder[0].c_str());
  TEST_ASSERT_EQUAL_STRING("persist:enter deep sleep", g_sleepOrder[1].c_str());
  TEST_ASSERT_EQUAL_STRING("sleepActivity", g_sleepOrder[2].c_str());
  TEST_ASSERT_EQUAL_STRING("after", g_sleepOrder[3].c_str());
}

// ---- Make* synthesizer tests ----

void test_make_go_to_reader_binds_payload() {
  registerRecordingFactories();
  ActivityRouter::setDepsForTest(ActivityRouter::Deps{});

  auto go = ActivityRouter::instance().makeGoToReader();
  go("/books/bar.epub");
  ActivityRouter::instance().applyIfPending();

  TEST_ASSERT_EQUAL_size_t(1, g_factoryCalls.size());
  TEST_ASSERT_EQUAL_STRING("Reader:/books/bar.epub", g_factoryCalls[0].c_str());
}

void test_make_go_home_has_no_payload() {
  registerRecordingFactories();
  ActivityRouter::Deps d{};
  d.persistAppState = &thunkPersist;
  d.trimSleepFolderIfDirty = &thunkTrim;
  ActivityRouter::setDepsForTest(d);

  auto go = ActivityRouter::instance().makeGoHome();
  go();
  ActivityRouter::instance().applyIfPending();

  TEST_ASSERT_EQUAL_size_t(1, g_factoryCalls.size());
  TEST_ASSERT_EQUAL_STRING("Home:", g_factoryCalls[0].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_policy_exhaustive_for_all_routes);
  RUN_TEST(test_request_home_persists_before_factory_runs);
  RUN_TEST(test_no_persist_routes_skip_persist);
  RUN_TEST(test_pending_coalesce_keeps_last_request);
  RUN_TEST(test_apply_if_pending_noop_without_pending);
  RUN_TEST(test_begin_dispatches_synchronously_with_payload);
  RUN_TEST(test_begin_home_applies_policy_before_factory);
  RUN_TEST(test_unmigrated_route_is_noop);
  RUN_TEST(test_enter_deep_sleep_sequence_from_reader);
  RUN_TEST(test_enter_deep_sleep_sequence_not_from_reader);
  RUN_TEST(test_make_go_to_reader_binds_payload);
  RUN_TEST(test_make_go_home_has_no_payload);
  return UNITY_END();
}
