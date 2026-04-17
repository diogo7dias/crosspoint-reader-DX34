#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// A simple two-option confirmation dialog (Confirm / Cancel).
class ConfirmDialogActivity final : public Activity {
 public:
  explicit ConfirmDialogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& message,
                                 const std::function<void()>& onConfirm, const std::function<void()>& onCancel)
      : Activity("ConfirmDialog", renderer, mappedInput), message(message), onConfirm(onConfirm), onCancel(onCancel) {}

  void onEnter() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  const std::string message;
  std::function<void()> onConfirm;
  std::function<void()> onCancel;
  ButtonNavigator buttonNavigator;
  int selectedOptionIndex = 1;  // Default to Cancel for safety
};
