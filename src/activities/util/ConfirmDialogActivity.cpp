#include "ConfirmDialogActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr int kOptionCount = 2;
}

void ConfirmDialogActivity::onEnter() {
  Activity::onEnter();
  selectedOptionIndex = 1;  // Default to Cancel
  requestUpdate();
}

void ConfirmDialogActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    auto cb = std::move(onCancel);
    if (cb) cb();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedOptionIndex = (selectedOptionIndex + 1) % kOptionCount;
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedOptionIndex = (selectedOptionIndex + kOptionCount - 1) % kOptionCount;
    requestUpdate();
  });

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedOptionIndex == 0) {
      auto cb = std::move(onConfirm);
      if (cb) cb();
    } else {
      auto cb = std::move(onCancel);
      if (cb) cb();
    }
  }
}

void ConfirmDialogActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const char* options[] = {tr(STR_CONFIRM), tr(STR_CANCEL)};
  constexpr int rowH = 28;
  const int popupW = pageWidth - 48;

  // Split message on newlines and truncate each line to fit popup width
  const int textMaxW = popupW - 24;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  std::vector<std::string> msgLines;
  {
    size_t start = 0;
    while (start <= message.size()) {
      auto nl = message.find('\n', start);
      if (nl == std::string::npos) nl = message.size();
      std::string line = message.substr(start, nl - start);
      msgLines.push_back(renderer.truncatedText(UI_10_FONT_ID, line.c_str(), textMaxW));
      start = nl + 1;
    }
  }
  const int textH = static_cast<int>(msgLines.size()) * (lineH + 2);

  const int popupH = 20 + textH + 16 + kOptionCount * rowH;
  const int popupX = (pageWidth - popupW) / 2;
  const int popupY = (pageHeight - popupH) / 2;

  renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
  renderer.fillRect(popupX, popupY, popupW, popupH, false);

  // Draw message lines
  int msgY = popupY + 10;
  for (const auto& line : msgLines) {
    renderer.drawText(UI_10_FONT_ID, popupX + 12, msgY, line.c_str(), true);
    msgY += lineH + 2;
  }

  // Draw options
  const int optionsStartY = popupY + 20 + textH + 8;
  for (int i = 0; i < kOptionCount; i++) {
    const int rowY = optionsStartY + i * rowH;
    const bool selected = (i == selectedOptionIndex);
    if (selected) {
      renderer.fillRect(popupX + 6, rowY - 1, popupW - 12, rowH - 2, true);
    }
    renderer.drawText(UI_10_FONT_ID, popupX + 12, rowY, options[i], !selected);
  }

  renderer.displayBuffer();
}
