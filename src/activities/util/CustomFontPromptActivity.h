#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Boot-time popup shown for each newly-detected /custom-font/<name>_<size>.bdf
// file. Three options:
//   Install      — caller persists filename in seenCustomFonts (Phase 1: log only).
//   Skip         — caller does nothing; popup re-appears next boot.
//   Skip forever — caller persists filename in skippedCustomFonts.
//
// The Back button maps to Skip (least-destructive default).
class CustomFontPromptActivity final : public Activity {
 public:
  explicit CustomFontPromptActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string message,
                                    std::function<void()> onInstall, std::function<void()> onSkip,
                                    std::function<void()> onSkipForever)
      : Activity("CustomFontPrompt", renderer, mappedInput),
        message(std::move(message)),
        onInstall(std::move(onInstall)),
        onSkip(std::move(onSkip)),
        onSkipForever(std::move(onSkipForever)) {}

  void onEnter() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  const std::string message;
  std::function<void()> onInstall;
  std::function<void()> onSkip;
  std::function<void()> onSkipForever;
  ButtonNavigator buttonNavigator;
  int selectedOptionIndex = 1;  // default to Skip (safe middle)
};
