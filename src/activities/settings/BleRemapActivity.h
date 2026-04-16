#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"

class BleRemapActivity final : public Activity {
 public:
  explicit BleRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onBack)
      : Activity("BleRemap", renderer, mappedInput), onBack(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  const std::function<void()> onBack;
  uint8_t currentStep = 0;
  static constexpr uint8_t kRoleCount = 6;
  uint16_t tempMapping[kRoleCount] = {};
  unsigned long errorUntil = 0;
  std::string errorMessage;

  void applyMapping();
  bool validateUnassigned(uint16_t keycode);
  const char* getRoleName(uint8_t step) const;
  static const char* getKeycodeName(uint16_t keycode);
};
