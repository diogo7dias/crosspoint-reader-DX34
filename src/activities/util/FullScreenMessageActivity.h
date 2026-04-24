#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontFamily::Style style;
  HalDisplay::RefreshMode refreshMode;
  // Optional: when set, Back / Confirm fires it so the caller can pop this
  // screen. Without it the activity is a dead-end — which is correct for
  // "SD card error" at boot but wrong for info popups like "No new fonts
  // found" launched from the settings menu.
  std::function<void()> onDismiss;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode) {}

  void setOnDismiss(std::function<void()> cb) { onDismiss = std::move(cb); }

  void onEnter() override;
  void loop() override;
};
