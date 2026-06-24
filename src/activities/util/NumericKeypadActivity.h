#pragma once

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// A minimal numeric entry screen: a single horizontal row of keys
// [0 1 2 3 4 5 6 7 8 9 < OK]. Next/Previous (Right/Left, also Down/Up) move the
// highlight, Confirm presses the key, Back cancels. The typed value is clamped
// to [0, maxValue]. On OK it reports the value via onComplete; Back reports
// cancellation via onCancel. Reusable wherever a number must be typed.
class NumericKeypadActivity final : public Activity {
 public:
  using OnComplete = std::function<void(long value)>;
  using OnCancel = std::function<void()>;

  NumericKeypadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, long maxValue,
                        long initialValue, OnComplete onComplete, OnCancel onCancel)
      : Activity("NumericKeypad", renderer, mappedInput),
        title(std::move(title)),
        maxValue(maxValue < 0 ? 0 : maxValue),
        onComplete(std::move(onComplete)),
        onCancel(std::move(onCancel)) {
    if (initialValue > 0) {
      digits = std::to_string(initialValue > this->maxValue ? this->maxValue : initialValue);
    }
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  // Apply a digit (0-9) or backspace (key == kBackspaceKey). OK is handled in
  // loop() so the activity can return immediately after onComplete.
  void applyDigitOrBackspace(int key);
  long currentValue() const;

  static constexpr int kKeyCount = 12;  // 0-9, backspace, OK
  static constexpr int kBackspaceKey = 10;
  static constexpr int kOkKey = 11;

  std::string title;
  long maxValue;
  OnComplete onComplete;
  OnCancel onCancel;

  ButtonNavigator buttonNavigator;
  std::string digits;    // typed digits (no separate leading-zero state)
  int selectedKey = 11;  // start on OK so a single Confirm accepts the initial value
};
