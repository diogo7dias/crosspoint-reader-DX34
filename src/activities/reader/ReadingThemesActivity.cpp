#include "ReadingThemesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <esp_heap_caps.h>

#include <algorithm>
#include <new>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReaderSamplePreview.h"
#include "ReaderSettingsActivity.h"
#include "ReadingThemeStore.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/TransitionFeedback.h"

namespace {
constexpr int kBaseRowCount = 3;
constexpr int kThemeActionCount = 5;

const char* themeActionLabel(const int index) {
  switch (index) {
    case 0:
      return tr(STR_THEME_APPLY);
    case 1:
      return tr(STR_THEME_RENAME_ACTION);
    case 2:
      return tr(STR_THEME_OVERWRITE);
    case 3:
      return tr(STR_THEME_DELETE_ACTION);
    case 4:
    default:
      return tr(STR_CANCEL);
  }
}
}  // namespace

void ReadingThemesActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  READING_THEMES.sortByName();
  selectedRowIndex = 0;
  requestUpdate();
}

void ReadingThemesActivity::onExit() { ActivityWithSubactivity::onExit(); }

int ReadingThemesActivity::rowCount() const { return kBaseRowCount + READING_THEMES.getCount(); }

bool ReadingThemesActivity::isThemeRow(const int rowIndex) const {
  return rowIndex >= kBaseRowCount && rowIndex < kBaseRowCount + READING_THEMES.getCount();
}

int ReadingThemesActivity::themeIndexForRow(const int rowIndex) const {
  return isThemeRow(rowIndex) ? rowIndex - kBaseRowCount : -1;
}

int ReadingThemesActivity::clampSelectedRow(const int rowIndex) const {
  return std::clamp(rowIndex, 0, rowCount() - 1);
}

void ReadingThemesActivity::showMessage(const std::string& message) {
  messagePopupText = message;
  messagePopupOpen = true;
  requestUpdate();
}

void ReadingThemesActivity::openKeyboardForNewTheme() {
  if (READING_THEMES.getCount() >= static_cast<int>(ReadingThemeStore::MAX_THEMES)) {
    showMessage(tr(STR_THEME_LIST_FULL));
    return;
  }

  const std::string suggestedName = READING_THEMES.makeUniqueName(tr(STR_THEME));
  exitActivity();
  enterNewActivity(new (std::nothrow) KeyboardEntryActivity(
      renderer, mappedInput, tr(STR_THEME_NAME), "", 10, ReadingThemeStore::MAX_THEME_NAME_LENGTH, false,
      [this, suggestedName](const std::string& name) {
        const std::string effective = name.empty() ? suggestedName : name;
        const bool ok = READING_THEMES.addTheme(effective);
        deferred_.post(ThemesAction::SubactivityExit);
        if (!ok) {
          pendingPostExitAction = [this]() { showMessage(tr(STR_SAVE_THEME_FAILED)); };
          return;
        }
        pendingPostExitAction = [this]() { selectedRowIndex = rowCount() - 1; };
      },
      [this]() { deferred_.post(ThemesAction::SubactivityExit); }, suggestedName));
}

void ReadingThemesActivity::openKeyboardForRename(const int themeIndex) {
  const ReadingTheme* theme = READING_THEMES.getTheme(themeIndex);
  if (theme == nullptr) {
    showMessage(tr(STR_THEME_NOT_FOUND));
    return;
  }

  exitActivity();
  enterNewActivity(new (std::nothrow) KeyboardEntryActivity(
      renderer, mappedInput, tr(STR_RENAME_THEME), "", 10, ReadingThemeStore::MAX_THEME_NAME_LENGTH, false,
      [this, themeIndex](const std::string& name) {
        if (name.empty()) {
          deferred_.post(ThemesAction::SubactivityExit);
          return;
        }
        const bool ok = READING_THEMES.renameTheme(themeIndex, name);
        deferred_.post(ThemesAction::SubactivityExit);
        pendingPostExitAction = [this, themeIndex, ok]() {
          actionPopupOpen = false;
          if (!ok) {
            showMessage(tr(STR_RENAME_THEME_FAILED));
            return;
          }
          selectedRowIndex = clampSelectedRow(kBaseRowCount + themeIndex);
        };
      },
      [this]() {
        deferred_.post(ThemesAction::SubactivityExit);
        pendingPostExitAction = [this]() { actionPopupOpen = false; };
      }));
}

void ReadingThemesActivity::executeThemeAction() {
  if (actionPopupThemeIndex < 0) {
    actionPopupOpen = false;
    return;
  }

  switch (actionPopupSelectedIndex) {
    case 0: {
      actionPopupOpen = false;
      TransitionFeedback::show(renderer, tr(STR_LOADING));
      if (!READING_THEMES.applyTheme(actionPopupThemeIndex, bookCachePath)) {
        TransitionFeedback::dismiss(renderer);
        showMessage(tr(STR_APPLY_THEME_FAILED));
        return;
      }
      settingsDirty = true;
      TransitionFeedback::dismiss(renderer);
      onClose(true);
      return;
    }
    case 1:
      openKeyboardForRename(actionPopupThemeIndex);
      return;
    case 2:
      actionPopupOpen = false;
      if (!READING_THEMES.updateTheme(actionPopupThemeIndex)) {
        showMessage(tr(STR_UPDATE_THEME_FAILED));
        return;
      }
      requestUpdate();
      return;
    case 3: {
      actionPopupOpen = false;
      const ReadingTheme* delTheme = READING_THEMES.getTheme(actionPopupThemeIndex);
      const std::string themeName = delTheme ? delTheme->name : "theme";
      const int delIndex = actionPopupThemeIndex;
      enterNewActivity(new (std::nothrow) ConfirmDialogActivity(
          renderer, mappedInput, std::string(tr(STR_DELETE_THEME_CONFIRM)) + "\n" + themeName,
          [this, delIndex]() {
            exitActivity();
            if (!READING_THEMES.deleteTheme(delIndex)) {
              showMessage(tr(STR_DELETE_THEME_FAILED));
              return;
            }
            selectedRowIndex = clampSelectedRow(selectedRowIndex);
            requestUpdate();
          },
          [this]() {
            exitActivity();
            requestUpdate();
          }));
      return;
    }
    case 4:
    default:
      actionPopupOpen = false;
      requestUpdate();
      return;
  }
}

void ReadingThemesActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    // Deferred exit: process after subActivity->loop() returns to avoid
    // use-after-free. SubactivityExit is the only action; the bit is cleared
    // before run(), matching the old "set flag false first" ordering.
    deferred_.drain([this](ThemesAction action) {
      if (action == ThemesAction::SubactivityExit) {
        const bool changed = pendingSettingsChanged;
        pendingSettingsChanged = false;
        auto postAction = std::move(pendingPostExitAction);
        pendingPostExitAction = nullptr;
        exitActivity();
        settingsDirty = settingsDirty || changed;
        if (postAction) postAction();
        requestUpdate();
      }
      return false;
    });
    return;
  }

  if (messagePopupOpen) {
    const bool anyPress = mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                          mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                          mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Right);
    if (anyPress) {
      messagePopupOpen = false;
      messagePopupText.clear();
      requestUpdate();
    }
    return;
  }

  if (previewPopupOpen) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      previewPopupOpen = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Apply the theme — same path as the action menu's Apply (index 0).
      previewPopupOpen = false;
      TransitionFeedback::show(renderer, tr(STR_LOADING));
      if (!READING_THEMES.applyTheme(previewPopupThemeIndex, bookCachePath)) {
        TransitionFeedback::dismiss(renderer);
        showMessage(tr(STR_APPLY_THEME_FAILED));
        return;
      }
      settingsDirty = true;
      TransitionFeedback::dismiss(renderer);
      onClose(true);
      return;
    }
    // Options (down/next button): open the Rename/Overwrite/Delete menu.
    buttonNavigator.onNextRelease([this] {
      previewPopupOpen = false;
      actionPopupOpen = true;
      actionPopupThemeIndex = previewPopupThemeIndex;
      actionPopupSelectedIndex = 0;
      requestUpdate();
    });
    return;
  }

  if (actionPopupOpen) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      actionPopupOpen = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      executeThemeAction();
      return;
    }
    buttonNavigator.onNextRelease([this] {
      actionPopupSelectedIndex = ButtonNavigator::nextIndex(actionPopupSelectedIndex, kThemeActionCount);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      actionPopupSelectedIndex = ButtonNavigator::previousIndex(actionPopupSelectedIndex, kThemeActionCount);
      requestUpdate();
    });
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onClose(settingsDirty);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedRowIndex == 0) {
      exitActivity();
      enterNewActivity(new (std::nothrow)
                           ReaderSettingsActivity(renderer, mappedInput, bookCachePath, [this](const bool changed) {
                             deferred_.post(ThemesAction::SubactivityExit);
                             pendingSettingsChanged = changed;
                           }));
      return;
    }
    if (selectedRowIndex == 1) {
      openKeyboardForNewTheme();
      return;
    }
    if (selectedRowIndex == 2) {
      enterNewActivity(new (std::nothrow) ConfirmDialogActivity(
          renderer, mappedInput, tr(STR_RESET_GLOBAL_STYLE_CONFIRM),
          [this]() {
            exitActivity();
            if (!READING_THEMES.resetBookSettingsToGlobal(bookCachePath)) {
              showMessage(tr(STR_FAILED_RESET_GLOBAL));
              return;
            }
            settingsDirty = true;
            showMessage(tr(STR_FOLLOWING_GLOBAL_STYLE));
          },
          [this]() {
            exitActivity();
            requestUpdate();
          }));
      return;
    }
    if (isThemeRow(selectedRowIndex)) {
      previewPopupOpen = true;
      previewPopupThemeIndex = themeIndexForRow(selectedRowIndex);
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedRowIndex = ButtonNavigator::nextIndex(selectedRowIndex, rowCount());
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedRowIndex = ButtonNavigator::previousIndex(selectedRowIndex, rowCount());
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this] {
    selectedRowIndex = ButtonNavigator::nextIndex(selectedRowIndex, rowCount());
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this] {
    selectedRowIndex = ButtonNavigator::previousIndex(selectedRowIndex, rowCount());
    requestUpdate();
  });
}

void ReadingThemesActivity::render(Activity::RenderLock&&) {
  TransitionFeedback::resetStacking();
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto metrics = BaseMetrics::values;

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + 5, tr(STR_READING_THEMES), true,
                            EpdFontFamily::REGULAR);

  const int currentThemeIndex = READING_THEMES.findMatchingTheme();
  const int lastAppliedThemeIndex = currentThemeIndex < 0 ? READING_THEMES.findLastAppliedTheme() : -1;
  const int contentY = metrics.topPadding + metrics.headerHeight;
  const int contentHeight = pageHeight - (contentY + metrics.buttonHintsHeight + metrics.verticalSpacing);
  const int rowHeight = metrics.listRowHeight;
  const int pageItems = std::max(1, contentHeight / rowHeight);
  const int pageStartIndex = (selectedRowIndex / pageItems) * pageItems;

  for (int i = pageStartIndex; i < rowCount() && i < pageStartIndex + pageItems; i++) {
    const int rowY = contentY + (i - pageStartIndex) * rowHeight;
    const bool isSelected = (i == selectedRowIndex);

    std::string label;
    if (i == 0) {
      label = tr(STR_ADJUST_CURRENT_SETTINGS);
    } else if (i == 1) {
      label = tr(STR_SAVE_CURRENT_AS_NEW);
    } else if (i == 2) {
      label = "Reset to Global Reader Style";
    } else {
      const int themeIndex = themeIndexForRow(i);
      const ReadingTheme* theme = READING_THEMES.getTheme(themeIndex);
      label = theme ? theme->name : tr(STR_THEME);
      if (themeIndex == currentThemeIndex) {
        label = "* " + label;
      } else if (themeIndex == lastAppliedThemeIndex) {
        label = "# " + label;
      }
    }

    if (isSelected) {
      renderer.fillRect(0, rowY, pageWidth, rowHeight, true);
    }
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, rowY, label.c_str(), !isSelected);

    if (i >= kBaseRowCount) {
      const int themeIndex = themeIndexForRow(i);
      const char* stateLabel = nullptr;
      if (themeIndex == currentThemeIndex) {
        stateLabel = tr(STR_CURRENT_THEME);
      } else if (themeIndex == lastAppliedThemeIndex) {
        stateLabel = tr(STR_LAST_USED_THEME);
      }
      if (stateLabel != nullptr) {
        const int currentW = renderer.getTextWidth(UI_10_FONT_ID, stateLabel);
        renderer.drawText(UI_10_FONT_ID, pageWidth - metrics.contentSidePadding - currentW, rowY, stateLabel,
                          !isSelected);
      }
    }
  }

  if (READING_THEMES.isEmpty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 70, tr(STR_NO_SAVED_THEMES));
  }

  {
    const std::string counter =
        std::to_string(READING_THEMES.getCount()) + " / " + std::to_string(ReadingThemeStore::MAX_THEMES);
    const int counterW = renderer.getTextWidth(UI_10_FONT_ID, counter.c_str());
    const int counterH = renderer.getLineHeight(UI_10_FONT_ID);
    const int counterY = pageHeight - metrics.buttonHintsHeight - 4 - counterH;
    renderer.drawText(UI_10_FONT_ID, pageWidth - metrics.contentSidePadding - counterW, counterY, counter.c_str());
  }

  const auto labels = previewPopupOpen
                          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_THEME_APPLY), "", tr(STR_THEME_ACTIONS))
                          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (previewPopupOpen) {
    const int popupW = pageWidth * 8 / 10;
    const int popupX = (pageWidth - popupW) / 2;
    const int popupY = pageHeight / 10;
    const int popupBottomMax = pageHeight - metrics.buttonHintsHeight - 6;
    const int popupH = std::min(pageHeight * 8 / 10, popupBottomMax - popupY);
    renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);  // border
    renderer.fillRect(popupX, popupY, popupW, popupH, false);                 // fill
    const ReadingTheme* theme = READING_THEMES.getTheme(previewPopupThemeIndex);
    const std::string title = theme ? theme->name : std::string(tr(STR_THEME));
    renderer.drawCenteredText(UI_12_FONT_ID, popupY + 8,
                              renderer.truncatedText(UI_12_FONT_ID, title.c_str(), popupW - 24).c_str(), true,
                              EpdFontFamily::REGULAR);
    const int sepY = popupY + 12 + renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawLine(popupX + 8, sepY, popupX + popupW - 8, sepY, true);
    const int sampleY = sepY + 6;
    const int sampleH = (popupY + popupH) - sampleY - 8;
    if (theme != nullptr && heap_caps_get_free_size(MALLOC_CAP_8BIT) >= 40 * 1024) {
      CrossPointSettings temp = SETTINGS;
      ReadingThemeStore::applyThemeToSettings(*theme, temp);
      // Capped box inset: exact px margins don't translate into a scaled popup
      // box, so wider-margin themes just read a little narrower here.
      constexpr int kPad = 8;
      const int usableW = std::max(1, popupW - 2 * kPad);
      const int margin = std::clamp<int>(temp.screenMarginHorizontal, 0, usableW / 3);
      const int insetL = popupX + kPad + margin;
      const int colW = std::max(1, popupW - 2 * (kPad + margin));
      reader::drawReaderSamplePreview(renderer, temp, insetL, colW, sampleY + kPad, sampleY + sampleH - kPad);
    } else {
      renderer.drawCenteredText(UI_10_FONT_ID, sampleY + sampleH / 2 - 8, "Preview unavailable (low memory)");
    }
  }

  if (actionPopupOpen) {
    const int popupW = pageWidth - 60;
    const int popupH = 62 + kThemeActionCount * 26;
    const int popupX = (pageWidth - popupW) / 2;
    const int popupY = (pageHeight - popupH) / 2;
    renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
    renderer.fillRect(popupX, popupY, popupW, popupH, false);
    renderer.drawCenteredText(UI_10_FONT_ID, popupY + 8, tr(STR_THEME_ACTIONS));
    const ReadingTheme* theme = READING_THEMES.getTheme(actionPopupThemeIndex);
    if (theme != nullptr) {
      const std::string themeName = renderer.truncatedText(UI_10_FONT_ID, theme->name.c_str(), popupW - 24);
      renderer.drawCenteredText(UI_10_FONT_ID, popupY + 24, themeName.c_str());
    }
    for (int i = 0; i < kThemeActionCount; i++) {
      const int rowY = popupY + 54 + i * 26;
      const bool isSelected = (i == actionPopupSelectedIndex);
      if (isSelected) {
        renderer.fillRect(popupX + 6, rowY - 1, popupW - 12, 24, true);
      }
      renderer.drawText(UI_10_FONT_ID, popupX + 12, rowY, themeActionLabel(i), !isSelected);
    }
  }

  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }

  renderer.displayBuffer();
}
