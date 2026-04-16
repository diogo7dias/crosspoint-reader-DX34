#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

void KeyboardEntryActivity::loop() {
  // Navigation
  if (keyboard.handleNavigation(buttonNavigator, mappedInput)) {
    requestUpdate();
  }

  // Confirm / select
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const auto result = keyboard.handleConfirm(text.length());
    applyKeyResult(result);
    requestUpdate();
  }

  // Cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (onCancel) onCancel();
    requestUpdate();
  }
}

void KeyboardEntryActivity::applyKeyResult(const KeyboardWidget::KeyResult& result) {
  switch (result.action) {
    case KeyboardWidget::KeyAction::Char:
      if (result.isDoubleTap && !text.empty()) {
        text.pop_back();  // Remove previous char before inserting alternate
      }
      if (maxLength == 0 || text.length() < maxLength) {
        text += result.character;
      }
      break;

    case KeyboardWidget::KeyAction::Space:
      if (maxLength == 0 || text.length() < maxLength) text += ' ';
      break;

    case KeyboardWidget::KeyAction::Backspace:
      if (!text.empty()) text.pop_back();
      break;

    case KeyboardWidget::KeyAction::Done:
      if (onComplete) onComplete(text);
      break;

    case KeyboardWidget::KeyAction::ShiftToggle:
    case KeyboardWidget::KeyAction::None:
      break;
  }
}

void KeyboardEntryActivity::render(Activity::RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  // Draw title
  renderer.drawCenteredText(UI_10_FONT_ID, startY, title.c_str());

  // Draw input field
  const int inputStartY = startY + renderer.getLineHeight(UI_10_FONT_ID) + 6;
  int inputEndY = inputStartY;
  renderer.drawText(UI_10_FONT_ID, 10, inputStartY, "[");

  std::string displayText;
  if (isPassword) {
    displayText = std::string(text.length(), '*');
  } else {
    displayText = text;
  }
  displayText += "_";

  // Render input text across multiple lines
  int lineStartIdx = 0;
  int lineEndIdx = displayText.length();
  while (true) {
    std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, lineText.c_str());
    if (textWidth <= pageWidth - 40) {
      renderer.drawText(UI_10_FONT_ID, 20, inputEndY, lineText.c_str());
      if (lineEndIdx == static_cast<int>(displayText.length())) break;

      inputEndY += renderer.getLineHeight(UI_10_FONT_ID);
      lineStartIdx = lineEndIdx;
      lineEndIdx = displayText.length();
    } else {
      lineEndIdx -= 1;
    }
  }
  renderer.drawText(UI_10_FONT_ID, pageWidth - 15, inputEndY, "]");

  // Draw keyboard
  const int keyboardStartY = inputEndY + 20;
  keyboard.render(renderer, keyboardStartY);

  // Draw help text
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer();
}
