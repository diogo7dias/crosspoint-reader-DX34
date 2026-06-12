// Host-side tests for the value-edit tap-step decision (settings popups).
//
// Pins the tap vs quick-double-tap semantics that drive line-spacing / margin
// editing: an isolated tap moves by 1, a tap within the double-tap window moves
// by 10, and rapid taps keep escalating. Driven by scripted timestamps, no
// hardware.
//
// Run via: pio test -e test_host -f test_value_edit_step
#include <unity.h>

#include "ValueEditStep.h"

using crosspoint::settings::kValueEditBigStep;
using crosspoint::settings::kValueEditUnitStep;
using crosspoint::settings::valueEditTapStep;

namespace {
constexpr unsigned long kWindow = 350;  // matches doubleTapMs in the activities
}

void setUp() {}
void tearDown() {}

void test_first_tap_no_history_is_unit_step() {
  // prevTapMs == 0 sentinel: nothing to double against.
  TEST_ASSERT_EQUAL_INT(kValueEditUnitStep, valueEditTapStep(0, 1000, kWindow));
}

void test_quick_second_tap_is_big_step() {
  // 200ms <= 350ms window -> double tap -> +10.
  TEST_ASSERT_EQUAL_INT(kValueEditBigStep, valueEditTapStep(1000, 1200, kWindow));
}

void test_gap_equal_to_window_is_big_step() {
  // Boundary is inclusive (mirrors the <= used for the menu double-tap).
  TEST_ASSERT_EQUAL_INT(kValueEditBigStep, valueEditTapStep(1000, 1350, kWindow));
}

void test_slow_tap_after_window_is_unit_step() {
  // 400ms > 350ms -> isolated tap -> +1.
  TEST_ASSERT_EQUAL_INT(kValueEditUnitStep, valueEditTapStep(1000, 1400, kWindow));
}

void test_rapid_taps_keep_escalating() {
  // Three taps each 150ms apart: 2nd and 3rd both land in-window -> +10 each.
  TEST_ASSERT_EQUAL_INT(kValueEditBigStep, valueEditTapStep(1000, 1150, kWindow));
  TEST_ASSERT_EQUAL_INT(kValueEditBigStep, valueEditTapStep(1150, 1300, kWindow));
}

void test_non_advancing_clock_is_unit_step() {
  // Defensive: equal/backwards timestamps must not report a giant gap.
  TEST_ASSERT_EQUAL_INT(kValueEditUnitStep, valueEditTapStep(1000, 1000, kWindow));
  TEST_ASSERT_EQUAL_INT(kValueEditUnitStep, valueEditTapStep(1000, 900, kWindow));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_tap_no_history_is_unit_step);
  RUN_TEST(test_quick_second_tap_is_big_step);
  RUN_TEST(test_gap_equal_to_window_is_big_step);
  RUN_TEST(test_slow_tap_after_window_is_unit_step);
  RUN_TEST(test_rapid_taps_keep_escalating);
  RUN_TEST(test_non_advancing_clock_is_unit_step);
  return UNITY_END();
}
