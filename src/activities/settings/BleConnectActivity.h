#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class BleConnectState : uint8_t {
  Initializing,
  Scanning,
  DeviceList,
  Connecting,
  Connected,
  Failed,
  UnpairConfirm
};

class BleConnectActivity final : public Activity {
 public:
  explicit BleConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              const std::function<void(bool paired)>& onComplete)
      : Activity("BleConnect", renderer, mappedInput), onComplete(onComplete) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  const std::function<void(bool paired)> onComplete;
  ButtonNavigator buttonNavigator;
  BleConnectState uiState = BleConnectState::Initializing;
  int selectedIndex = 0;
  unsigned long stateEnteredAt = 0;

  void renderInitializing() const;
  void renderScanning() const;
  void renderDeviceList() const;
  void renderConnecting() const;
  void renderConnected() const;
  void renderFailed() const;
  void renderUnpairConfirm() const;
  const char* signalIndicator(int rssi) const;
};
