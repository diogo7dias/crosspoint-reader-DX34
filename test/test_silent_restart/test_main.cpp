// Host tests for the SilentRestart pre-restart hook registry + runner.
//
// armAndRestart() itself is device-only (RTC_NOINIT + ESP.restart()), but the
// hook registry and runPreRestartHooks() are header-only and pure, so the
// "every silent reboot flushes pending state" contract is testable on the host
// without faking the reboot. The production wiring registers one hook that calls
// PersistManager().flushAll(); here we assert the runner invokes registered
// hooks, in order, honouring the fixed capacity.
//
// Run via: pio test -e test_host -f test_silent_restart
#include <unity.h>

#include "SilentRestart.h"

static int g_a;
static int g_b;
static int g_order;
static int g_aOrder;
static int g_bOrder;

void setUp() {
  clearPreRestartHooksForTest();
  g_a = 0;
  g_b = 0;
  g_order = 0;
  g_aOrder = 0;
  g_bOrder = 0;
}
void tearDown() { clearPreRestartHooksForTest(); }

void test_no_hooks_is_noop() {
  runPreRestartHooks();  // must not crash with an empty registry
  TEST_ASSERT_EQUAL_INT(0, g_a);
}

void test_registered_hook_runs_once() {
  registerPreRestartHook([]() { g_a++; });
  runPreRestartHooks();
  TEST_ASSERT_EQUAL_INT(1, g_a);
}

void test_hooks_run_in_registration_order() {
  registerPreRestartHook([]() { g_aOrder = ++g_order; });
  registerPreRestartHook([]() { g_bOrder = ++g_order; });
  runPreRestartHooks();
  TEST_ASSERT_EQUAL_INT(1, g_aOrder);
  TEST_ASSERT_EQUAL_INT(2, g_bOrder);
}

void test_null_hook_ignored() {
  registerPreRestartHook(nullptr);
  registerPreRestartHook([]() { g_a++; });
  runPreRestartHooks();
  TEST_ASSERT_EQUAL_INT(1, g_a);  // null skipped, real hook still ran
}

void test_capacity_cap_drops_excess() {
  for (size_t i = 0; i < kMaxPreRestartHooks + 3; ++i) {
    registerPreRestartHook([]() { g_a++; });
  }
  runPreRestartHooks();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(kMaxPreRestartHooks), g_a);
}

void test_runner_is_idempotent_across_calls() {
  registerPreRestartHook([]() { g_a++; });
  runPreRestartHooks();
  runPreRestartHooks();
  TEST_ASSERT_EQUAL_INT(2, g_a);  // each run fires the registered hooks again
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_hooks_is_noop);
  RUN_TEST(test_registered_hook_runs_once);
  RUN_TEST(test_hooks_run_in_registration_order);
  RUN_TEST(test_null_hook_ignored);
  RUN_TEST(test_capacity_cap_drops_excess);
  RUN_TEST(test_runner_is_idempotent_across_calls);
  return UNITY_END();
}
