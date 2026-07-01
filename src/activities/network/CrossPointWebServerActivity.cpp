#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <qrcode.h>

#include <cstddef>
#include <memory>
#include <new>

#include "MappedInputManager.h"
#include "WifiSelectionActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "network/WifiTeardown.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;

// DNS server for captive portal (redirects all DNS queries to our IP)
std::unique_ptr<DNSServer> dnsServer;
constexpr uint16_t DNS_PORT = 53;

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  // Lector is station-mode only (hotspot/Calibre removed): skip the old
  // network-mode menu and go straight to Wi-Fi selection. The browser web page is
  // the file-transfer + firmware-update path.
  state = WebServerActivityState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  LOG_DBG("WEBACT", "Turning on WiFi (STA mode)...");
  WiFi.mode(WIFI_STA);
  enterNewActivity(new (std::nothrow) WifiSelectionActivity(
      renderer, mappedInput, [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void CrossPointWebServerActivity::onExit() {
  // Sample before any teardown: getMode() is unreliable post-WIFI_OFF. Covers
  // both AP (getMode()==WIFI_AP) and STA (status()==WL_CONNECTED) sessions.
  const bool wifiWasUp = (WiFi.status() == WL_CONNECTED) || (WiFi.getMode() != WIFI_MODE_NULL);

  ActivityWithSubactivity::onExit();

  state = WebServerActivityState::SHUTTING_DOWN;

  // Stop the web server first (before disconnecting WiFi)
  stopWebServer();

  // Stop mDNS
  MDNS.end();

  // Stop DNS server if running (AP mode)
  if (dnsServer) {
    LOG_DBG("WEBACT", "Stopping DNS server...");
    dnsServer->stop();
    dnsServer.reset();
  }

  // Brief wait for LWIP stack to flush pending packets before the radio drops
  delay(50);

  net::teardownAndReclaim(wifiWasUp, net::WifiRestartTarget::Home, "wifi-exit-CrossPointWebServer", isApMode);
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();

    exitActivity();

    // mDNS responder skipped for v2.0.0 — its ~6.5 KB resident commit was
    // pushing the STA-mode heap pool below the fragmentation floor needed
    // for parallel asset fetches on phone browsers. Users open the device
    // by typed IP; the IP is shown on screen after pairing.

    // Start the web server
    startWebServer();
  } else {
    // User cancelled Wi-Fi selection — return home.
    exitActivity();
    onGoBack();
  }
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Create the web server instance
  webServer.reset(new (std::nothrow) CrossPointWebServer());
  if (!webServer) {
    LOG_ERR("WEBACT", "OOM new CrossPointWebServer — staying in starting state");
    return;
  }
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    {
      RenderLock lock(*this);
      render(std::move(lock));
    }
    LOG_DBG("WEBACT", "Rendered File Transfer screen");
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    onGoBack();
  }
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    LOG_DBG("WEBACT", "Stopping web server...");
    webServer->stop();
    LOG_DBG("WEBACT", "Web server stopped");
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  if (subActivity) {
    // Forward loop to subactivity
    subActivity->loop();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: watch the link state for UX, never for an auto-exit.
    // A flickering router or a user briefly moving out of range used
    // to kick the session into SHUTTING_DOWN and hang the activity;
    // we now just toggle a banner and let the WiFi stack auto-
    // reconnect in the background. handleClient() below happily
    // no-ops while the link is down.
    if (!isApMode) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoBack) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoBack();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            // IP may have changed on DHCP renewal — refresh the label.
            const auto ip = WiFi.localIP();
            char ipStr[16];
            snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            connectedIP = ipStr;
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      esp_task_wdt_reset();

      // Process HTTP requests in tight loop for maximum throughput
      // More iterations = more data processed per main loop cycle
      constexpr int MAX_ITERATIONS = 150;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        // Check Back button before the first handleClient() and every 32 iters
        // after. Prior cadence (every 64) made Exit unresponsive; every 16
        // measurably slowed concurrent WebSocket uploads. 32 is the middle
        // ground.
        if (i == 0 || (i & 0x1F) == 0x1F) {
          yield();
          mappedInput.update();
          if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            onGoBack();
            return;
          }
          esp_task_wdt_reset();
        }
        webServer->handleClient();
      }
      lastHandleClientTime = millis();
    }

    // Handle exit on Back button (also check outside loop)
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoBack();
      return;
    }
  }
}

void CrossPointWebServerActivity::render(Activity::RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING) {
    renderer.clearScreen();
    renderServerRunning();
    renderer.displayBuffer(nextRefreshMode);
    nextRefreshMode = HalDisplay::FAST_REFRESH;
  } else if (state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, tr(STR_STARTING_HOTSPOT), true,
                              EpdFontFamily::REGULAR);
    renderer.displayBuffer(nextRefreshMode);
    nextRefreshMode = HalDisplay::FAST_REFRESH;
  }
}

void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data) {
  // Implementation of QR code calculation
  // The structure to manage the QR code
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  LOG_DBG("WEBACT", "QR Code (%lu): %s", data.length(), data.c_str());

  qrcode_initText(&qrcode, qrcodeBytes, 4, ECC_LOW, data.c_str());
  const uint8_t px = 6;  // pixels per module
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        renderer.fillRect(x + px * cx, y + px * cy, px, px, true);
      }
    }
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  // Use consistent line spacing
  constexpr int LINE_SPACING = 28;  // Space between lines

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_FILE_TRANSFER), true, EpdFontFamily::REGULAR);

  if (!isApMode) {
    renderWifiIndicator(15);
  }

  if (isApMode) {
    // AP mode display - center the content block
    int startY = 55;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_HOTSPOT_MODE), true, EpdFontFamily::REGULAR);

    std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + connectedSSID;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ssidInfo.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 2, tr(STR_CONNECT_WIFI_HINT));

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, tr(STR_SCAN_QR_WIFI_HINT));
    // Show QR code for URL
    const std::string wifiConfig = std::string("WIFI:S:") + connectedSSID + ";;";
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 4, wifiConfig);

    startY += 6 * 29 + 3 * LINE_SPACING;
    // Show primary URL (hostname)
    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str(), true,
                              EpdFontFamily::REGULAR);

    // Show IP address as fallback
    std::string ipUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + connectedIP + "/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, ipUrl.c_str());
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, tr(STR_OPEN_URL_HINT));

    // Show QR code for URL
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 6, tr(STR_SCAN_QR_HINT));
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 7, hostnameUrl);
  } else {
    // STA mode display (original behavior)
    const int startY = 65;

    std::string ssidInfo = std::string(tr(STR_NETWORK_PREFIX)) + connectedSSID;
    if (ssidInfo.length() > 28) {
      ssidInfo.replace(25, ssidInfo.length() - 25, "...");
    }
    renderer.drawCenteredText(UI_10_FONT_ID, startY, ssidInfo.c_str());

    std::string ipInfo = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING, ipInfo.c_str());

    // Show web server URL prominently
    std::string webInfo = "http://" + connectedIP + "/";
    renderer.drawCenteredText(UI_10_FONT_ID, startY + LINE_SPACING * 2, webInfo.c_str(), true, EpdFontFamily::REGULAR);

    // Also show hostname URL
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 3, hostnameUrl.c_str());

    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 4, tr(STR_OPEN_URL_HINT));

    // Show QR code for URL
    drawQRCode(renderer, (480 - 6 * 33) / 2, startY + LINE_SPACING * 6, webInfo);
    renderer.drawCenteredText(SMALL_FONT_ID, startY + LINE_SPACING * 5, tr(STR_SCAN_QR_HINT));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // STA-only link-loss banner. Sits under the title so it doesn't
  // push the QR code around. Disappears automatically when the link
  // comes back.
  if (consecutiveDisconnects > 0 && !isApMode) {
    renderer.drawCenteredText(SMALL_FONT_ID, 36, "Reconnecting to Wi-Fi...", true, EpdFontFamily::REGULAR);
  }
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  // DX34 has no UITheme metrics; use hardcoded layout to anchor indicator
  // in the top-right corner near the title.
  constexpr int CONTENT_SIDE_PADDING = 12;
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - CONTENT_SIDE_PADDING;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + ICON_HEIGHT;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
