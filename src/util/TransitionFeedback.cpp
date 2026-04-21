#include "TransitionFeedback.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "StringUtils.h"
#include "fontIds.h"

namespace TransitionFeedback {
namespace {
bool sActive = false;
int sBottomY = 0;
// Timestamp of the FIRST toast in the current stack. Stacked toasts don't
// reset it, so it measures cumulative stack age, which we use as the basis
// for kStillWorkingThresholdMs.
unsigned long sShownAtMs = 0;
bool sStillWorkingShown = false;
}  // namespace

void show(GfxRenderer& renderer, const char* message) {
  if (!message || message[0] == '\0') {
    sActive = false;
    return;
  }

  // If no popup is currently active, this call opens a NEW stack. Reset
  // the timestamp + still-working latch here so stale values from a
  // previous context (e.g. a StatusPopup during folder navigation) can't
  // leak into the next threshold check. Without this, sShownAtMs would
  // retain a timestamp from minutes ago and maybeShowStillWorkingToast()
  // would fire the "Long chapter..." popup instantly.
  if (!sActive) {
    sShownAtMs = millis();
    sStillWorkingShown = false;
  }

  const std::string upper = StringUtils::toUpperAscii(message);

  const int screenW = renderer.getScreenWidth();
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, upper.c_str(), EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);

  constexpr int paddingX = 20;
  constexpr int paddingY = 12;
  constexpr int border = 2;
  const int boxW = textWidth + paddingX * 2;
  const int boxH = textHeight + paddingY * 2;
  const int boxX = (screenW - boxW) / 2;
  const int boxY = sActive ? (sBottomY + kGap) : kStartY;

  renderer.fillRect(boxX - border, boxY - border, boxW + border * 2, boxH + border * 2, true);
  renderer.fillRect(boxX, boxY, boxW, boxH, false);

  const int textX = boxX + (boxW - textWidth) / 2;
  const int textY = boxY + paddingY - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, upper.c_str(), true, EpdFontFamily::REGULAR);

  // FAST_REFRESH diffs new framebuffer vs previous (in RED RAM): only pixels
  // inside the popup rectangle flip, so feedback appears in ~400 ms instead
  // of the ~1700 ms a HALF_REFRESH takes. The popup's pixels are NOT written
  // into RED RAM after a fast refresh, so the next fast refresh would
  // compare the destination activity's buffer against the pre-popup frame
  // and leave visible ghosting where the popup sat. Request a HALF on the
  // next displayBuffer so the destination's first render fully rewrites
  // both BW and RED RAM and scrubs any ghost.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  renderer.requestHalfRefresh();
  sBottomY = boxY + boxH + border;
  sActive = true;
}

void dismiss(GfxRenderer& renderer) {
  (void)renderer;
  sActive = false;
  sBottomY = 0;
  sShownAtMs = 0;
  sStillWorkingShown = false;
}

void ensureMinDisplayElapsed() {
  if (!sActive || sShownAtMs == 0) {
    return;
  }
  const unsigned long now = millis();
  const unsigned long elapsed = now - sShownAtMs;
  if (elapsed < kMinDisplayMs) {
    delay(kMinDisplayMs - elapsed);
  }
}

bool isActive() { return sActive; }

int bottomY() { return sActive ? sBottomY : 0; }

void resetStacking() {
  sActive = false;
  sBottomY = 0;
  sShownAtMs = 0;
  sStillWorkingShown = false;
}

void markOpenComplete() { sStillWorkingShown = false; }

void maybeShowStillWorkingToast(GfxRenderer& renderer) {
  // No toast on screen yet? Nothing to stack below — the "Opening book..."
  // popup is the reference timer, so if it hasn't been drawn there's
  // nothing to measure.
  if (!sActive || sShownAtMs == 0 || sStillWorkingShown) {
    return;
  }
  const unsigned long elapsed = millis() - sShownAtMs;
  if (elapsed >= kStillWorkingThresholdMs) {
    show(renderer, "Long chapter...Still opening...");
    sStillWorkingShown = true;
  }
}

}  // namespace TransitionFeedback
