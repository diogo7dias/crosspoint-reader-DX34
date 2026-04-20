#include "TransitionFeedback.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "StringUtils.h"
#include "fontIds.h"

namespace TransitionFeedback {
namespace {
bool sActive = false;
int sBottomY = 0;
}  // namespace

void show(GfxRenderer& renderer, const char* message) {
  if (!message || message[0] == '\0') {
    sActive = false;
    return;
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
  if (!sActive) {
    return;
  }
  sActive = false;
  sBottomY = 0;
  renderer.clearScreen();
  // HALF_REFRESH at dismiss time scrubs any popup ghost in a single pass;
  // we only reach dismiss when no destination render is coming to clean up.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool isActive() { return sActive; }

int bottomY() { return sActive ? sBottomY : 0; }

void resetStacking() {
  sActive = false;
  sBottomY = 0;
}

}  // namespace TransitionFeedback
