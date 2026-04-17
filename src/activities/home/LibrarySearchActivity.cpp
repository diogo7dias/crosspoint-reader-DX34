#include "LibrarySearchActivity.h"

#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>

#include "LibrarySearchSupport.h"
#include "MappedInputManager.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

LibrarySearchActivity::LibrarySearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string folderPath, const std::vector<std::string>& entries,
                                             std::string initialQuery, OnCompleteCallback onComplete,
                                             OnCancelCallback onCancel)
    : Activity("LibrarySearch", renderer, mappedInput),
      folderPath(std::move(folderPath)),
      entries(entries),
      query(std::move(initialQuery)),
      onComplete(std::move(onComplete)),
      onCancel(std::move(onCancel)) {}

void LibrarySearchActivity::onEnter() {
  Activity::onEnter();
  rebuildPreviewMatches();
  requestUpdate();
}

void LibrarySearchActivity::onExit() { Activity::onExit(); }

void LibrarySearchActivity::rebuildPreviewMatches() {
  previewMatches = LibrarySearchSupport::rankMatches(entries, query);
}

void LibrarySearchActivity::applyKeyResult(const KeyboardWidget::KeyResult& result) {
  switch (result.action) {
    case KeyboardWidget::KeyAction::Char:
      if (result.isDoubleTap && !query.empty()) {
        query.pop_back();
      }
      if (query.size() < MAX_QUERY_LENGTH) {
        query += result.character;
      }
      rebuildPreviewMatches();
      break;

    case KeyboardWidget::KeyAction::Space:
      if (query.size() < MAX_QUERY_LENGTH) {
        query += ' ';
        rebuildPreviewMatches();
      }
      break;

    case KeyboardWidget::KeyAction::Backspace:
      if (!query.empty()) {
        query.pop_back();
        rebuildPreviewMatches();
      }
      break;

    case KeyboardWidget::KeyAction::Done:
      if (onComplete) onComplete(query);
      break;

    case KeyboardWidget::KeyAction::ShiftToggle:
    case KeyboardWidget::KeyAction::None:
      break;
  }
}

void LibrarySearchActivity::loop() {
  // Navigation
  if (keyboard.handleNavigation(buttonNavigator, mappedInput)) {
    requestUpdate();
  }

  // Confirm / select (use wasPressed for double-tap timing)
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const auto result = keyboard.handleConfirm(query.length());
    applyKeyResult(result);
    requestUpdate();
  }

  // Back = finish search with current query
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (onComplete) onComplete(query);
  }
}

void LibrarySearchActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_10_FONT_ID, 10, "Search current folder");

  const int pathY = 30;
  const int pathWidth = pageWidth - 24;
  const std::string pathLabel = renderer.truncatedText(SMALL_FONT_ID, folderPath.c_str(), pathWidth);
  renderer.drawText(SMALL_FONT_ID, 12, pathY, pathLabel.c_str());

  const int queryBoxY = pathY + renderer.getLineHeight(SMALL_FONT_ID) + 8;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int queryBoxH = lineH + 10;
  renderer.drawRect(10, queryBoxY, pageWidth - 20, queryBoxH, true);
  std::string displayQuery = query.empty() ? "_" : (query + "_");
  displayQuery = renderer.truncatedText(UI_10_FONT_ID, displayQuery.c_str(), pageWidth - 34);
  renderer.drawText(UI_10_FONT_ID, 16, queryBoxY + 5, displayQuery.c_str());

  const int previewStartY = queryBoxY + queryBoxH + 8;

  // Keyboard anchored to bottom, above button hints
  const int btnHintsHeight = BaseMetrics::values.buttonHintsHeight;
  const int keyboardStartY = pageHeight - btnHintsHeight - 8 - keyboard.getTotalHeight(renderer);

  // Match summary and preview results between query box and keyboard
  std::string matchSummary = query.empty() ? "Type to search" : (std::to_string(previewMatches.size()) + " matches");
  renderer.drawText(UI_10_FONT_ID, 12, previewStartY, matchSummary.c_str());

  const int previewLineH = renderer.getLineHeight(UI_10_FONT_ID) + 2;
  int previewY = previewStartY + renderer.getLineHeight(UI_10_FONT_ID) + 4;
  const int availablePreviewRows = std::max(0, (keyboardStartY - 8 - previewY) / previewLineH);
  const int rowsToShow = std::min(availablePreviewRows, static_cast<int>(previewMatches.size()));
  for (int i = 0; i < rowsToShow; ++i) {
    const std::string label = renderer.truncatedText(
        UI_10_FONT_ID, LibrarySearchSupport::searchLabelForEntry(entries[previewMatches[i]]).c_str(), pageWidth - 24);
    renderer.drawText(UI_10_FONT_ID, 12, previewY, label.c_str());
    previewY += previewLineH;
  }

  // Draw keyboard
  keyboard.render(renderer, keyboardStartY);

  // Button hints
  const auto labels = mappedInput.mapLabels(tr(STR_OK_BUTTON), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
