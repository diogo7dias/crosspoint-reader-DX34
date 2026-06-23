#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "network/WifiTeardown.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
  }
  requestUpdate();

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = KOReaderSyncClient::authenticate();

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage = KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Turn on WiFi
  WiFi.mode(WIFI_STA);

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    state = AUTHENTICATING;
    statusMessage = tr(STR_AUTHENTICATING);
    requestUpdate();

    // Perform authentication in a separate task
    if (xTaskCreate(
            [](void* param) {
              auto* self = static_cast<KOReaderAuthActivity*>(param);
              self->performAuthentication();
              vTaskDelete(nullptr);
            },
            "AuthTask", 4096, this, 1, nullptr) != pdPASS) {
      LOG_ERR("KOAuth", "Failed to create AuthTask — heap exhausted");
      state = FAILED;
      statusMessage = "Task creation failed";
      requestUpdate();
    }
    return;
  }

  // Launch WiFi selection
  enterNewActivity(new (std::nothrow) WifiSelectionActivity(
      renderer, mappedInput, [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void KOReaderAuthActivity::onExit() {
  // Sample before teardown: getMode() is unreliable once WIFI_OFF is set.
  const bool wifiWasUp = (WiFi.status() == WL_CONNECTED) || (WiFi.getMode() != WIFI_MODE_NULL);

  ActivityWithSubactivity::onExit();

  net::teardownAndReclaim(wifiWasUp, net::WifiRestartTarget::Home, "wifi-exit-KOReaderAuth");
}

void KOReaderAuthActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_KOREADER_AUTH), true, EpdFontFamily::REGULAR);

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, 300, statusMessage.c_str(), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, tr(STR_SYNC_READY));

    const auto labels = mappedInput.mapLabels(tr(STR_DONE), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, 280, tr(STR_AUTH_FAILED), true, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, 320, errorMessage.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void KOReaderAuthActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      onComplete();
    }
  }
}
