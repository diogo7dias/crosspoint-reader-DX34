#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <string>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

// The stable mDNS address the web-server activity advertises; reachable as soon
// as the device is on WiFi with the web page open. Mirrors AP_HOSTNAME in
// CrossPointWebServerActivity.cpp.
namespace {
constexpr const char* kWebUpdateHostUrl = "http://crosspoint.local/update";
}  // namespace

void OtaUpdateActivity::onEnter() { requestUpdate(); }

void OtaUpdateActivity::loop() {
  // Either button just dismisses the info screen — there is nothing to confirm.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    goBack();
  }
}

void OtaUpdateActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_UPDATE), true, EpdFontFamily::REGULAR);

  const bool connected = WiFi.status() == WL_CONNECTED;

  // Bordered notice: Lector has no over-the-air update. The whole message lives
  // inside one framed box; the box height is derived from the final text cursor
  // so it always wraps the content exactly.
  const int boxX = 18;
  const int boxW = renderer.getScreenWidth() - 2 * boxX;
  const int boxY = static_cast<int>(renderer.getScreenHeight() * 0.16f);
  int y = boxY + 22;

  renderer.drawCenteredText(UI_10_FONT_ID, y, "No over-the-air (OTA) update", true, EpdFontFamily::REGULAR);
  y += 34;
  renderer.drawCenteredText(UI_10_FONT_ID, y, "in this firmware.", true, EpdFontFamily::REGULAR);
  y += 46;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Update from a browser:");
  y += 34;
  renderer.drawCenteredText(UI_10_FONT_ID, y, kWebUpdateHostUrl, true, EpdFontFamily::REGULAR);
  y += 32;
  if (connected) {
    const std::string ipUrl = std::string("or http://") + WiFi.localIP().toString().c_str() + "/update";
    renderer.drawCenteredText(SMALL_FONT_ID, y, ipUrl.c_str());
    y += 30;
  }
  y += 8;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Start the page in Settings > WiFi.");
  y += 30;

  const int boxH = (y + 12) - boxY;
  renderer.drawRect(boxX, boxY, boxW, boxH, 2, true);

  renderer.drawCenteredText(SMALL_FONT_ID, boxY + boxH + 24,
                            (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
