/**
 * Host-side conformance tests for the IAsyncRunner port, exercised through
 * the synchronous InlineRunner implementation.
 *
 * These tests pin the contract the production FreeRTOS-backed runner
 * (AsyncWriter) and any future implementations must honour:
 *   - start() is idempotent.
 *   - submit() invokes the callable exactly once.
 *   - drainBlocking() returns without hanging when nothing is queued.
 *   - droppedCount() is monotonic and zero on a healthy runner.
 *
 * Runs on the developer machine via:
 *     pio test -e test_host -f test_async_runner
 */
#include <unity.h>

#include <functional>

#include "persist/IAsyncRunner.h"
#include "persist/InlineRunner.h"
#include "persist/PersistManager.h"

using crosspoint::persist::IAsyncRunner;
using crosspoint::persist::InlineRunner;
using crosspoint::persist::PersistManager;
using crosspoint::persist::PersistManagerImpl;
using crosspoint::persist::WriteMode;

namespace {
InlineRunner g_runner;
}

void setUp() { g_runner.resetForTest(); }
void tearDown() {}

void test_start_is_idempotent() {
  g_runner.start();
  g_runner.start();
  g_runner.start();
  TEST_ASSERT_TRUE(g_runner.started());
}

void test_submit_runs_callable_synchronously() {
  g_runner.start();
  int counter = 0;
  g_runner.submit([&counter]() { counter = 42; });
  // Inline runner: by the time submit() returns, the callable has run.
  TEST_ASSERT_EQUAL_INT(42, counter);
  TEST_ASSERT_EQUAL_size_t(1, g_runner.submittedCount());
}

void test_submit_runs_each_callable_exactly_once() {
  g_runner.start();
  int calls = 0;
  for (int i = 0; i < 5; ++i) {
    g_runner.submit([&calls]() { ++calls; });
  }
  TEST_ASSERT_EQUAL_INT(5, calls);
  TEST_ASSERT_EQUAL_size_t(5, g_runner.submittedCount());
}

void test_submit_empty_function_is_safe() {
  g_runner.start();
  g_runner.submit(std::function<void()>{});  // null target
  // Just must not crash. Submitted counter still advances; the runner
  // recorded an attempt, which is the contract: best-effort acceptance.
  TEST_ASSERT_EQUAL_size_t(1, g_runner.submittedCount());
}

void test_drainBlocking_returns_when_idle() {
  g_runner.start();
  // Nothing pending — must return immediately.
  g_runner.drainBlocking();
  TEST_ASSERT_TRUE(g_runner.started());
}

void test_droppedCount_is_zero_on_healthy_runner() {
  g_runner.start();
  for (int i = 0; i < 100; ++i) {
    g_runner.submit([]() {});
  }
  TEST_ASSERT_EQUAL_size_t(0, g_runner.droppedCount());
}

void test_iasyncrunner_polymorphic_use() {
  // The port is the public surface; tests must work through it.
  IAsyncRunner& port = g_runner;
  port.start();
  bool ran = false;
  port.submit([&ran]() { ran = true; });
  port.drainBlocking();
  TEST_ASSERT_TRUE(ran);
  TEST_ASSERT_EQUAL_size_t(0, port.droppedCount());
}

// ===== PersistManager::requestFlush — Sync + Async dispatch =====

namespace {

PersistManagerImpl& freshPM() {
  PersistManagerImpl& pm = PersistManager();
  pm.clearForTest();
  pm.setAsyncRunnerForTest(nullptr);
  return pm;
}

}  // namespace

void test_requestFlush_sync_store_runs_inline() {
  PersistManagerImpl& pm = freshPM();
  int flushCount = 0;
  pm.registerStore(PersistManagerImpl::Entry{
      [](uint32_t) { return false; },
      [&flushCount]() {
        ++flushCount;
        return true;
      },
      []() { return false; },
      "sync_store",
      "/sync.json",
      WriteMode::Sync,
  });

  pm.requestFlush("sync_store");
  TEST_ASSERT_EQUAL_INT(1, flushCount);
}

void test_requestFlush_async_store_uses_runner() {
  PersistManagerImpl& pm = freshPM();
  InlineRunner inline_;
  pm.setAsyncRunnerForTest(&inline_);

  int flushCount = 0;
  pm.registerStore(PersistManagerImpl::Entry{
      [](uint32_t) { return false; },
      [&flushCount]() {
        ++flushCount;
        return true;
      },
      []() { return false; },
      "async_store",
      "/async.json",
      WriteMode::Async,
  });

  pm.requestFlush("async_store");
  // InlineRunner runs the submission synchronously, so flushNow has
  // executed by the time requestFlush returns.
  TEST_ASSERT_EQUAL_INT(1, flushCount);
  TEST_ASSERT_EQUAL_size_t(1, inline_.submittedCount());
}

void test_requestFlush_unknown_name_is_noop() {
  PersistManagerImpl& pm = freshPM();
  InlineRunner inline_;
  pm.setAsyncRunnerForTest(&inline_);
  int flushCount = 0;
  pm.registerStore(PersistManagerImpl::Entry{
      [](uint32_t) { return false; },
      [&flushCount]() {
        ++flushCount;
        return true;
      },
      []() { return false; },
      "real_store",
      "/real.json",
      WriteMode::Async,
  });

  pm.requestFlush("does_not_exist");
  TEST_ASSERT_EQUAL_INT(0, flushCount);
  TEST_ASSERT_EQUAL_size_t(0, inline_.submittedCount());
}

void test_requestFlush_async_fallback_to_sync_when_no_runner() {
  PersistManagerImpl& pm = freshPM();
  // No runner bound: host build with no test seam.
  int flushCount = 0;
  pm.registerStore(PersistManagerImpl::Entry{
      [](uint32_t) { return false; },
      [&flushCount]() {
        ++flushCount;
        return true;
      },
      []() { return false; },
      "async_store",
      "/async.json",
      WriteMode::Async,
  });

  pm.requestFlush("async_store");
  // Falls back to sync so data is never lost in host builds.
  TEST_ASSERT_EQUAL_INT(1, flushCount);
}

void test_requestFlush_sync_is_default_mode() {
  PersistManagerImpl& pm = freshPM();
  InlineRunner inline_;
  pm.setAsyncRunnerForTest(&inline_);
  int flushCount = 0;
  // Note: mode omitted from designated init — defaults to Sync.
  pm.registerStore(PersistManagerImpl::Entry{
      [](uint32_t) { return false; },
      [&flushCount]() {
        ++flushCount;
        return true;
      },
      []() { return false; },
      "default_store",
      "/default.json",
  });

  pm.requestFlush("default_store");
  TEST_ASSERT_EQUAL_INT(1, flushCount);
  // Sync path: runner must not have been consulted.
  TEST_ASSERT_EQUAL_size_t(0, inline_.submittedCount());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_is_idempotent);
  RUN_TEST(test_submit_runs_callable_synchronously);
  RUN_TEST(test_submit_runs_each_callable_exactly_once);
  RUN_TEST(test_submit_empty_function_is_safe);
  RUN_TEST(test_drainBlocking_returns_when_idle);
  RUN_TEST(test_droppedCount_is_zero_on_healthy_runner);
  RUN_TEST(test_iasyncrunner_polymorphic_use);
  RUN_TEST(test_requestFlush_sync_store_runs_inline);
  RUN_TEST(test_requestFlush_async_store_uses_runner);
  RUN_TEST(test_requestFlush_unknown_name_is_noop);
  RUN_TEST(test_requestFlush_async_fallback_to_sync_when_no_runner);
  RUN_TEST(test_requestFlush_sync_is_default_mode);
  return UNITY_END();
}
