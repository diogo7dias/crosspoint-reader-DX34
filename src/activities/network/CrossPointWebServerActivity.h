#pragma once

#include <functional>
#include <memory>
#include <string>

#include "HalDisplay.h"
#include "activities/ActivityWithSubactivity.h"
#include "network/CrossPointWebServer.h"

// Web server activity states
enum class WebServerActivityState {
  MODE_SELECTION,  // Choosing between Join Network and Create Hotspot
  WIFI_SELECTION,  // WiFi selection subactivity is active (for Join Network mode)
  AP_STARTING,     // Starting Access Point mode
  SERVER_RUNNING,  // Web server is running and handling requests
  SHUTTING_DOWN    // Shutting down server and WiFi
};

/**
 * CrossPointWebServerActivity is the entry point for file transfer functionality.
 * It:
 * - First presents a choice between "Join a Network" (STA), "Connect to Calibre", and "Create Hotspot" (AP)
 * - For STA mode: Launches WifiSelectionActivity to connect to an existing network
 * - For AP mode: Creates an Access Point that clients can connect to
 * - Starts the CrossPointWebServer when connected
 * - Handles client requests in its loop() function
 * - Cleans up the server and shuts down WiFi on exit
 */
class CrossPointWebServerActivity final : public ActivityWithSubactivity {
  WebServerActivityState state = WebServerActivityState::WIFI_SELECTION;
  const std::function<void()> onGoBack;

  // Lector is station-mode only (hotspot/AP removed). Kept as a const false so the
  // few remaining STA/AP-shared render helpers read cleanly; never set true.
  static constexpr bool isApMode = false;

  // Web server - owned by this activity
  std::unique_ptr<CrossPointWebServer> webServer;

  // Server status
  std::string connectedIP;
  std::string connectedSSID;  // For STA mode: network name, For AP mode: AP name

  // Display refresh control — first render uses HALF_REFRESH to clear ghosting
  HalDisplay::RefreshMode nextRefreshMode = HalDisplay::HALF_REFRESH;

  // Performance monitoring
  unsigned long lastHandleClientTime = 0;

  // Sustained WiFi-loss tracking; abandon only after WIFI_ABANDON_MS.
  // While consecutiveDisconnects>0, render shows the "Reconnecting…" banner.
  int consecutiveDisconnects = 0;
  unsigned long firstDisconnectAt = 0;
  static constexpr unsigned long WIFI_ABANDON_MS = 5UL * 60UL * 1000UL;

  // Cached signal-strength bracket (0..4) for the WiFi indicator.
  int lastWifiBars = 0;

  void renderServerRunning() const;
  void renderWifiIndicator(int subHeaderTop) const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();

 public:
  explicit CrossPointWebServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("CrossPointWebServer", renderer, mappedInput), onGoBack(onGoBack) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  // User explicitly asked for no implicit session ending: while this
  // activity is foreground we block auto-sleep regardless of whether
  // the web server happens to be running right now (transient WiFi
  // drops, mid-start window, etc.). Only Back or power-button-hold
  // exits.
  bool preventAutoSleep() override { return true; }
};
