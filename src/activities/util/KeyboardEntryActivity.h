#pragma once
#include <GfxRenderer.h>

#include <functional>
#include <string>
#include <utility>

#include "../Activity.h"
#include "util/ButtonNavigator.h"
#include "util/KeyboardWidget.h"

/**
 * Reusable keyboard entry activity for text input.
 * Uses KeyboardWidget for dual-label keyboard with double-tap support.
 *
 * Back-as-OK: if the user has typed (text != initialText), the Back button
 * submits instead of canceling — faster save for edits.
 * If text is unchanged, Back cancels.
 *
 * Placeholder: shown in the input field while `text` is empty, to hint at a
 * suggested value without forcing the user to clear it.
 */
class KeyboardEntryActivity : public Activity {
 public:
  using OnCompleteCallback = std::function<void(const std::string&)>;
  using OnCancelCallback = std::function<void()>;

  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "", const int startY = 10,
                                 const size_t maxLength = 0, const bool isPassword = false,
                                 OnCompleteCallback onComplete = nullptr, OnCancelCallback onCancel = nullptr,
                                 std::string placeholder = "")
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        startY(startY),
        text(initialText),
        initialText(std::move(initialText)),
        placeholder(std::move(placeholder)),
        maxLength(maxLength),
        isPassword(isPassword),
        onComplete(std::move(onComplete)),
        onCancel(std::move(onCancel)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  std::string title;
  int startY;
  std::string text;
  std::string initialText;
  std::string placeholder;
  size_t maxLength;
  bool isPassword;

  ButtonNavigator buttonNavigator;
  KeyboardWidget keyboard;

  OnCompleteCallback onComplete;
  OnCancelCallback onCancel;

  void applyKeyResult(const KeyboardWidget::KeyResult& result);
};
