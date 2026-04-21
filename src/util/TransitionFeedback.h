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

// Default top Y position for the first popup.
constexpr int kStartY = 60;
// Gap between stacked popups.
constexpr int kGap = 8;
// Minimum visible display time for a popup, in milliseconds.
constexpr unsigned long kMinDisplayMs = 500;

}  // namespace TransitionFeedback
