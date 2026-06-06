#include "network/WifiTeardown.h"

#include <Arduino.h>
#include <WiFi.h>

#include "SilentRestart.h"

namespace {
// Unified settle time around the disconnect/power-down (call sites previously
// used 30, 50, or 100 ms ad hoc). 50 ms is long enough for the disconnect
// frame to flush and the radio to power down on the C3, while keeping a
// no-restart back-out (the only case where the delay is user-visible) snappy.
constexpr unsigned kWifiSettleMs = 50;
}  // namespace

void net::teardownAndReclaim(bool wifiWasUp, WifiRestartTarget target, const char* reason, bool apMode) {
  if (apMode) {
    WiFi.softAPdisconnect(true);
  } else {
    WiFi.disconnect(false);  // false = keep credentials, send disconnect frame
  }
  delay(kWifiSettleMs);
  WiFi.mode(WIFI_OFF);
  delay(kWifiSettleMs);

  if (!wifiWasUp) {
    return;
  }
  if (target == WifiRestartTarget::Reader) {
    silentRestartToReader(reason);  // does not return
  } else {
    silentRestart(reason);  // does not return
  }
}
