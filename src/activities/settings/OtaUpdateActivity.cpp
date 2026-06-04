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

  int y = static_cast<int>(renderer.getScreenHeight() * 0.22f);
  renderer.drawCenteredText(UI_10_FONT_ID, y, "Update from your browser", true, EpdFontFamily::REGULAR);
  y += 60;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Open Settings > WiFi to start the web");
  y += 26;
  renderer.drawCenteredText(SMALL_FONT_ID, y, "page, then open this address in a browser:");
  y += 56;
  renderer.drawCenteredText(UI_10_FONT_ID, y, kWebUpdateHostUrl, true, EpdFontFamily::REGULAR);
  y += 36;
  if (connected) {
    const std::string ipUrl = std::string("or http://") + WiFi.localIP().toString().c_str() + "/update";
    renderer.drawCenteredText(SMALL_FONT_ID, y, ipUrl.c_str());
    y += 30;
  }
  y += 40;
  renderer.drawCenteredText(SMALL_FONT_ID, y, (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
