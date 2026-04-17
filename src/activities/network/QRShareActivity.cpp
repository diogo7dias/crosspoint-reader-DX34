#include "QRShareActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <qrcode.h>

#include "WifiSelectionActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {

void drawQRCode(const GfxRenderer& renderer, const int x, const int y, const std::string& data) {
  QRCode qrcode;
  uint8_t qrcodeBytes[qrcode_getBufferSize(4)];
  qrcode_initText(&qrcode, qrcodeBytes, 4, ECC_LOW, data.c_str());
  const uint8_t px = 6;
  for (uint8_t cy = 0; cy < qrcode.size; cy++) {
    for (uint8_t cx = 0; cx < qrcode.size; cx++) {
      if (qrcode_getModule(&qrcode, cx, cy)) {
        renderer.fillRect(x + px * cx, y + px * cy, px, px, true);
      }
    }
  }
}

std::string extractFileName(const std::string& path) {
  const auto pos = path.rfind('/');
  if (pos != std::string::npos && pos + 1 < path.size()) {
    return path.substr(pos + 1);
  }
  return path;
}

bool isEpubFile(const std::string& name) {
  if (name.size() < 5) return false;
  std::string ext = name.substr(name.size() - 5);
  for (auto& c : ext) c = tolower(c);
  return ext == ".epub";
}

}  // namespace

QRShareActivity::QRShareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onGoBack, std::string filePath)
    : ActivityWithSubactivity("QRShare", renderer, mappedInput),
      onGoBack(onGoBack),
      filePath(std::move(filePath)),
      fileName(extractFileName(this->filePath)) {}

void QRShareActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  const bool alreadyConnected = (WiFi.status() == WL_CONNECTED) || (WiFi.getMode() == WIFI_AP);
  if (alreadyConnected) {
    startServerAndShowQR();
  } else {
    // Need to connect to WiFi first — launch selection subactivity
    LOG_INF("QRSHARE", "WiFi not connected, launching WiFi selection...");
    WiFi.mode(WIFI_STA);
    state = QRShareState::WIFI_SELECTION;
    enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                               [this](const bool connected) { onWifiSelectionComplete(connected); }));
  }
}

void QRShareActivity::onWifiSelectionComplete(const bool connected) {
  exitActivity();  // exit WifiSelectionActivity subactivity

  if (connected) {
    startServerAndShowQR();
  } else {
    // User cancelled WiFi selection — disconnect and go back
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
    delay(30);
    state = QRShareState::NO_WIFI;
    onGoBack();
  }
}

void QRShareActivity::startServerAndShowQR() {
  if (startServer()) {
    String ip = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    serverUrl = "http://" + std::string(ip.c_str()) + "/";
    state = QRShareState::QR_DISPLAY;
  } else {
    state = QRShareState::QR_DISPLAY;  // render will show error
  }
  requestUpdate();
}

void QRShareActivity::onExit() {
  stopServer();

  // If we started WiFi for this share, clean it up
  if (WiFi.status() == WL_CONNECTED || WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
    delay(30);
  }

  ActivityWithSubactivity::onExit();
}

void QRShareActivity::loop() {
  // Let subactivity (WifiSelectionActivity) handle its own loop
  if (subActivity) {
    ActivityWithSubactivity::loop();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
    return;
  }
  if (serverRunning && server) {
    for (int i = 0; i < 200; i++) {
      server->handleClient();
      if (i % 32 == 0) esp_task_wdt_reset();
      yield();
    }
  }
}

void QRShareActivity::render(Activity::RenderLock&&) {
  // Don't render when subactivity is active (it handles its own rendering)
  if (state == QRShareState::WIFI_SELECTION) return;

  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();

  if (!serverRunning) {
    renderer.drawCenteredText(UI_12_FONT_ID, 200, "Failed to start server", true, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(UI_10_FONT_ID, 240, "Port 80 may be in use.");
  } else {
    constexpr int LINE_SPACING = 28;
    int y = 30;

    renderer.drawCenteredText(UI_12_FONT_ID, y, "Share File", true, EpdFontFamily::REGULAR);
    y += LINE_SPACING + 4;

    std::string displayName = renderer.truncatedText(UI_10_FONT_ID, fileName.c_str(), screenW - 48);
    renderer.drawCenteredText(UI_10_FONT_ID, y, displayName.c_str());
    y += LINE_SPACING + 8;

    // QR code: version 4 at 6px/module = 33 modules * 6 = 198px wide
    constexpr int qrSize = 33 * 6;
    const int qrX = (screenW - qrSize) / 2;
    drawQRCode(renderer, qrX, y, serverUrl);
    y += qrSize + 16;

    renderer.drawCenteredText(SMALL_FONT_ID, y, "Scan with your phone to download");
    y += LINE_SPACING;

    renderer.drawCenteredText(UI_10_FONT_ID, y, serverUrl.c_str(), true, EpdFontFamily::REGULAR);
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

bool QRShareActivity::startServer() {
  server = std::make_unique<WebServer>(80);
  server->on("/", HTTP_GET, [this] { handleDownload(); });
  server->begin();
  serverRunning = true;
  LOG_INF("QRSHARE", "Server started for: %s", filePath.c_str());
  return true;
}

void QRShareActivity::stopServer() {
  if (server) {
    serverRunning = false;
    server->stop();
    server.reset();
    LOG_INF("QRSHARE", "Server stopped");
  }
}

void QRShareActivity::handleDownload() {
  if (!Storage.exists(filePath.c_str())) {
    server->send(404, "text/plain", "File not found");
    return;
  }

  FsFile file = Storage.open(filePath.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server->send(500, "text/plain", "Failed to open file");
    return;
  }

  String contentType = isEpubFile(fileName) ? "application/epub+zip" : "application/octet-stream";

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + String(fileName.c_str()) + "\"");
  server->send(200, contentType.c_str(), "");

  char buf[4096];
  while (file.available()) {
    esp_task_wdt_reset();
    const int bytesRead = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (bytesRead <= 0) break;
    server->sendContent(buf, bytesRead);
    yield();
  }
  file.close();
  server->sendContent("");
  LOG_INF("QRSHARE", "Download complete: %s", fileName.c_str());
}
