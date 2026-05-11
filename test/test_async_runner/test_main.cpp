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

using crosspoint::persist::IAsyncRunner;
using crosspoint::persist::InlineRunner;

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_is_idempotent);
  RUN_TEST(test_submit_runs_callable_synchronously);
  RUN_TEST(test_submit_runs_each_callable_exactly_once);
  RUN_TEST(test_submit_empty_function_is_safe);
  RUN_TEST(test_drainBlocking_returns_when_idle);
  RUN_TEST(test_droppedCount_is_zero_on_healthy_runner);
  RUN_TEST(test_iasyncrunner_polymorphic_use);
  return UNITY_END();
}
