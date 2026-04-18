// Host-side tests for crosspoint::network::SettingsGateway (RFC #24).
//
// Run via:  pio test -e test_host -f test_settings_gateway
//
// The gateway is a thin orchestrator — these tests verify its two bug-fix
// invariants (count propagation + disk-error reporting). The large
// key-dispatch logic lives in an injected ApplyFn and is exercised on
// device, not here.

#include <ArduinoJson.h>
#include <unity.h>

#include "network/SettingsGateway.h"

using crosspoint::network::SettingsGateway;

void setUp() {}
void tearDown() {}

void test_happy_path_applies_and_persists() {
  int applyCalls = 0;
  int persistCalls = 0;
  SettingsGateway g(
      [&applyCalls](const JsonDocument&) {
        applyCalls++;
        return 3;
      },
      [&persistCalls]() {
        persistCalls++;
        return true;
      });

  JsonDocument doc;
  doc["any"] = "value";
  auto r = g.applyJson(doc);

  TEST_ASSERT_EQUAL(1, applyCalls);
  TEST_ASSERT_EQUAL(1, persistCalls);
  TEST_ASSERT_EQUAL(3, r.applied);
  TEST_ASSERT_TRUE(r.persisted);
  TEST_ASSERT_TRUE(r.error.empty());
}

void test_disk_error_populates_error_message() {
  SettingsGateway g([](const JsonDocument&) { return 5; }, []() { return false; });

  JsonDocument doc;
  auto r = g.applyJson(doc);

  TEST_ASSERT_EQUAL(5, r.applied);  // apply still ran
  TEST_ASSERT_FALSE(r.persisted);
  TEST_ASSERT_FALSE(r.error.empty());
}

void test_zero_applied_still_calls_persist() {
  // Matches legacy behavior: even if no keys were recognized, saveToFile is
  // still invoked. Preserving that contract means the 500-on-disk-error fix
  // works the same whether or not the body had known keys.
  int persistCalls = 0;
  SettingsGateway g([](const JsonDocument&) { return 0; },
                    [&persistCalls]() {
                      persistCalls++;
                      return true;
                    });

  JsonDocument doc;
  auto r = g.applyJson(doc);

  TEST_ASSERT_EQUAL(1, persistCalls);
  TEST_ASSERT_EQUAL(0, r.applied);
  TEST_ASSERT_TRUE(r.persisted);
}

void test_null_apply_fn_defaults_to_zero() {
  // Defensive: gateway should not crash if a caller forgets to wire apply.
  SettingsGateway g(nullptr, []() { return true; });
  JsonDocument doc;
  auto r = g.applyJson(doc);
  TEST_ASSERT_EQUAL(0, r.applied);
  TEST_ASSERT_TRUE(r.persisted);
}

void test_null_persist_fn_reports_failure() {
  SettingsGateway g([](const JsonDocument&) { return 1; }, nullptr);
  JsonDocument doc;
  auto r = g.applyJson(doc);
  TEST_ASSERT_EQUAL(1, r.applied);
  TEST_ASSERT_FALSE(r.persisted);
  TEST_ASSERT_FALSE(r.error.empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_happy_path_applies_and_persists);
  RUN_TEST(test_disk_error_populates_error_message);
  RUN_TEST(test_zero_applied_still_calls_persist);
  RUN_TEST(test_null_apply_fn_defaults_to_zero);
  RUN_TEST(test_null_persist_fn_reports_failure);
  return UNITY_END();
}
