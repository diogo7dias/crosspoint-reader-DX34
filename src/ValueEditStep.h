#pragma once

// Pure decision for the value-edit popup (line spacing, margins, ...): how far a
// single tap moves the value. An isolated tap nudges by unitStep; a quick second
// tap landing within doubleTapWindowMs of the previous tap in the SAME direction
// is treated as a "double tap" and jumps by bigStep. Rapid repeats keep stepping
// by bigStep (each tap inside the window escalates), which is what lets the user
// fly through a long range with a burst of taps while a slow tap stays fine.
//
// Holding the button is handled separately by the caller as a steady unitStep
// repeat — this helper only governs discrete release/tap edges.
namespace crosspoint::settings {

constexpr int kValueEditUnitStep = 1;
constexpr int kValueEditBigStep = 10;
// Max gap between two taps for the second to count as a quick double tap. Shared
// by both settings menus; matches the menu-navigation double-tap window.
constexpr unsigned long kValueEditDoubleTapMs = 350;

inline int valueEditTapStep(unsigned long prevTapMs, unsigned long nowMs, unsigned long doubleTapWindowMs,
                            int unitStep = kValueEditUnitStep, int bigStep = kValueEditBigStep) {
  // prevTapMs == 0 is the "no prior tap" sentinel. Require a strictly positive
  // gap so a non-advancing/backwards clock or a same-millisecond duplicate edge
  // (button bounce) can never be mistaken for a deliberate quick double tap.
  if (prevTapMs != 0 && nowMs > prevTapMs && (nowMs - prevTapMs) <= doubleTapWindowMs) {
    return bigStep;
  }
  return unitStep;
}

}  // namespace crosspoint::settings
