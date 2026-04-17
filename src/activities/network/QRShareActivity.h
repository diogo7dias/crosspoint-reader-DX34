#pragma once
#include <WebServer.h>

#include <functional>
#include <memory>
#include <string>

#include "../ActivityWithSubactivity.h"

enum class QRShareState {
  WIFI_SELECTION,  // WiFi selection subactivity active
  QR_DISPLAY,      // QR code shown, server running
  NO_WIFI,         // WiFi cancelled or failed
};

class QRShareActivity final : public ActivityWithSubactivity {
 public:
  explicit QRShareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                           const std::function<void()>& onGoBack, std::string filePath);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool skipLoopDelay() override { return serverRunning; }
  bool preventAutoSleep() override { return serverRunning; }

 private:
  const std::function<void()> onGoBack;
  const std::string filePath;
  std::string fileName;
  std::string serverUrl;
  std::unique_ptr<WebServer> server;
  bool serverRunning = false;
  QRShareState state = QRShareState::WIFI_SELECTION;

  void onWifiSelectionComplete(bool connected);
  void startServerAndShowQR();
  bool startServer();
  void stopServer();
  void handleDownload();
};
