#pragma once

class GfxRenderer;

// Unified notification popup system for the e-ink display.
// All popups are drawn top-aligned and stack downward automatically —
// calling show() while another popup is active places the new popup
// below the previous one, giving multi-stage progress feedback.
// show() draws a popup instantly (FAST_REFRESH).
// dismiss() clears ghosting with a HALF_REFRESH before the next screen.
// resetStacking() clears the stacking state without a screen refresh —
// use before a full repaint so the next popup starts at the top again.
namespace TransitionFeedback {

void show(GfxRenderer& renderer, const char* message);

void dismiss(GfxRenderer& renderer);
bool isActive();

// Block (via delay) until at least kMinDisplayMs have elapsed since the
// last show(). No-op if already elapsed or if no popup is active.
// Call right before the next screen render when an operation completed
// faster than the min-display floor.
void ensureMinDisplayElapsed();

// Bottom edge (Y) of the last drawn popup, for stacking other popups below.
int bottomY();

// Clear stacking state without touching the display.
// Call before a full screen repaint so the next popup starts at startY.
void resetStacking();

// Idempotent: stack "Long chapter...Still opening..." once, if
// kStillWorkingThresholdMs has elapsed since the FIRST toast in the current
// stack was drawn (i.e. since "Opening book..." first appeared). Safe to
// call from tight loops — only triggers a display refresh on the threshold
// crossing. No-op if no toast is currently active, the threshold hasn't
// been crossed, or the toast has already fired.
void maybeShowStillWorkingToast(GfxRenderer& renderer);

// Clear the still-working latch without touching the popup stack or
// display. Use after a successful first render on code paths that don't
// call dismiss() (e.g. XtcReaderActivity, which visually replaces the
// popup via renderPage's full-frame draw) so page-turn renders after the
// first don't accidentally re-trigger the toast.
void markOpenComplete();

// Default top Y position for the first popup.
constexpr int kStartY = 60;
// Gap between stacked popups.
constexpr int kGap = 8;
// Minimum visible display time for a popup, in milliseconds.
constexpr unsigned long kMinDisplayMs = 500;
// How long after the first toast ("Opening book...") was drawn before the
// "Long chapter...Still opening..." reassurance toast stacks below.
constexpr unsigned long kStillWorkingThresholdMs = 6000;

}  // namespace TransitionFeedback
