#include "ReadingThemesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <new>

#include "MappedInputManager.h"
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
        pendingSubactivityExit = true;
        if (!ok) {
          pendingPostExitAction = [this]() { showMessage(tr(STR_SAVE_THEME_FAILED)); };
          return;
        }
        pendingPostExitAction = [this]() { selectedRowIndex = rowCount() - 1; };
      },
      [this]() { pendingSubactivityExit = true; }, suggestedName));
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
          pendingSubactivityExit = true;
          return;
        }
        const bool ok = READING_THEMES.renameTheme(themeIndex, name);
        pendingSubactivityExit = true;
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
        pendingSubactivityExit = true;
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
    // use-after-free
    if (pendingSubactivityExit) {
      pendingSubactivityExit = false;
      const bool changed = pendingSettingsChanged;
      pendingSettingsChanged = false;
      auto postAction = std::move(pendingPostExitAction);
      pendingPostExitAction = nullptr;
      exitActivity();
      settingsDirty = settingsDirty || changed;
      if (postAction) postAction();
      requestUpdate();
    }
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
                             pendingSubactivityExit = true;
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
      actionPopupOpen = true;
      actionPopupThemeIndex = themeIndexForRow(selectedRowIndex);
      actionPopupSelectedIndex = 0;
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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

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
