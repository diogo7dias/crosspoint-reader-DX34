#include "NumericKeypadActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdlib>
#include <string>

#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

void NumericKeypadActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void NumericKeypadActivity::onExit() { Activity::onExit(); }

long NumericKeypadActivity::currentValue() const {
  if (digits.empty()) return 0;
  long v = std::strtol(digits.c_str(), nullptr, 10);
  if (v > maxValue) v = maxValue;
  if (v < 0) v = 0;
  return v;
}

void NumericKeypadActivity::applyDigitOrBackspace(int key) {
  if (key == kBackspaceKey) {
    if (!digits.empty()) digits.pop_back();
    return;
  }
  if (key < 0 || key > 9) return;
  const char c = static_cast<char>('0' + key);
  // Replace a lone "0" rather than building "0X"; otherwise append.
  std::string cand = (digits == "0") ? std::string(1, c) : digits + c;
  // Bound the length to the digit count of maxValue so strtol can't overflow.
  if (cand.size() > std::to_string(maxValue).size()) return;
  const long v = std::strtol(cand.c_str(), nullptr, 10);
  digits = (v > maxValue) ? std::to_string(maxValue) : cand;
}

void NumericKeypadActivity::loop() {
  buttonNavigator.onNextRelease([this] {
    selectedKey = ButtonNavigator::nextIndex(selectedKey, kKeyCount);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedKey = ButtonNavigator::previousIndex(selectedKey, kKeyCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedKey == kOkKey) {
      // onComplete typically tears this activity down — return immediately,
      // touch no members afterwards.
      if (onComplete) onComplete(currentValue());
      return;
    }
    applyDigitOrBackspace(selectedKey);
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onCancel) onCancel();
    return;
  }
}

void NumericKeypadActivity::render(Activity::RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // Title.
  const int titleY = 40;
  renderer.drawCenteredText(UI_10_FONT_ID, titleY, title.c_str());

  // Current value, large and centered.
  const std::string valueText = digits.empty() ? "0" : digits;
  const int valueY = titleY + renderer.getLineHeight(UI_10_FONT_ID) + 24;
  renderer.drawCenteredText(UI_12_FONT_ID, valueY, valueText.c_str());

  // Single horizontal key row.
  const int margin = 8;
  const int avail = pageWidth - 2 * margin;
  const int keyW = avail / kKeyCount;
  const int keyH = 46;
  const int gap = 2;
  const int rowY = valueY + renderer.getLineHeight(UI_12_FONT_ID) + 28;
  for (int i = 0; i < kKeyCount; ++i) {
    const int x = margin + i * keyW;
    const bool selected = (i == selectedKey);
    if (selected) {
      renderer.fillRect(x, rowY, keyW - gap, keyH);
    } else {
      renderer.drawRect(x, rowY, keyW - gap, keyH);
    }
    std::string label;
    if (i <= 9) {
      label = std::to_string(i);
    } else if (i == kBackspaceKey) {
      label = "<";  // backspace
    } else {
      label = "OK";
    }
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, label.c_str());
    const int labelX = x + (keyW - gap - labelW) / 2;
    const int labelY = rowY + (keyH - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawText(UI_10_FONT_ID, labelX, labelY, label.c_str(), !selected);
  }

  (void)pageHeight;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
