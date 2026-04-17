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

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
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
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool isActive() { return sActive; }

int bottomY() { return sActive ? sBottomY : 0; }

void resetStacking() {
  sActive = false;
  sBottomY = 0;
}

}  // namespace TransitionFeedback
