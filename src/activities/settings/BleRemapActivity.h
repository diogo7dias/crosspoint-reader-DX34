#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"

class BleRemapActivity final : public Activity {
 public:
  explicit BleRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : Activity("BleRemap", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  enum class UiMode : uint8_t {
    List,     // Navigate the role list, pick one to bind or clear
    Waiting,  // Waiting for a BLE keypress to bind to selectedRole
  };

  const std::function<void()> onBack;
  static constexpr uint8_t kRoleCount = 6;

  UiMode uiMode = UiMode::List;
  uint8_t selectedRole = 0;
  uint16_t tempMapping[kRoleCount] = {};
  unsigned long errorUntil = 0;
  std::string errorMessage;

  void commitMapping();
  bool validateUnassigned(uint16_t keycode);
  void showError(const char* msg);
  const char* getRoleName(uint8_t role) const;
  static const char* getKeycodeName(uint16_t keycode);
};
