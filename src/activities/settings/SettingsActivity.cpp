#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "CleanupStorageActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FontFamilyApply.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "SettingsList.h"
#include "SleepWallpaperListActivity.h"
#include "ValueEditStep.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "util/TransitionFeedback.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_STATUS_BAR, StrId::STR_CAT_CONTROLS,
                                                              StrId::STR_CAT_SYSTEM};

namespace {
constexpr unsigned long doubleTapMs = 350;

// Render the X3 clock's biased quarter-hour UTC offset (48 = UTC+0, 0 = UTC-12,
// 104 = UTC+14) as a human label like "UTC+1", "UTC-5:30". Used by the VALUE
// renderer so the picker shows a timezone, not a raw 0..104 number.
std::string formatUtcOffset(uint8_t biasedQuarterHours) {
  const int totalMinutes = (static_cast<int>(biasedQuarterHours) - 48) * 15;
  const int absMinutes = totalMinutes < 0 ? -totalMinutes : totalMinutes;
  std::string out = "UTC";
  out += (totalMinutes < 0) ? '-' : '+';
  out += std::to_string(absMinutes / 60);
  const int minutes = absMinutes % 60;
  if (minutes != 0) {
    out += ':';
    if (minutes < 10) out += '0';
    out += std::to_string(minutes);
  }
  return out;
}

void persistSettingsWithLog(const char* context) {
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("SET", "Failed to save settings (%s)", context);
  }
}

std::string fontSizeValueLabel(const uint8_t family, const uint8_t fontSize) {
  return std::to_string(CrossPointSettings::fontSizeToPointSize(family, fontSize));
}

}  // namespace

const std::vector<SettingInfo>* SettingsActivity::settingsForCategory(const int categoryIndex) const {
  switch (categoryIndex) {
    case 0:
      return &displaySettings;
    case 1:
      return &readerSettings;
    case 2:
      return &statusBarSettings;
    case 3:
      return &controlsSettings;
    case 4:
    default:
      return &systemSettings;
  }
}

int SettingsActivity::findNextEditableRow(const int startIndex, const int direction) const {
  if (flatRows.empty()) {
    return 0;
  }
  int idx = startIndex;
  for (size_t i = 0; i < flatRows.size(); i++) {
    idx = (direction > 0) ? ButtonNavigator::nextIndex(idx, static_cast<int>(flatRows.size()))
                          : ButtonNavigator::previousIndex(idx, static_cast<int>(flatRows.size()));
    if (!flatRows[idx].isHeader) {
      return idx;
    }
  }
  return startIndex;
}

void SettingsActivity::jumpCategory(const int direction) {
  if (categoryHeaderRowIndices.empty()) {
    return;
  }
  int currentCategory = flatRows[selectedRowIndex].categoryIndex;
  int targetCategory = currentCategory;
  for (int i = 0; i < categoryCount; i++) {
    targetCategory = (direction > 0) ? ButtonNavigator::nextIndex(targetCategory, categoryCount)
                                     : ButtonNavigator::previousIndex(targetCategory, categoryCount);
    const auto* settings = settingsForCategory(targetCategory);
    if (settings && !settings->empty()) {
      break;
    }
  }
  const int headerRow = categoryHeaderRowIndices[targetCategory];
  selectedRowIndex = findNextEditableRow(headerRow, +1);
}

bool SettingsActivity::isPopupValueSetting(const SettingInfo& setting) const {
  if (setting.type != SettingType::VALUE || setting.valuePtr == nullptr) {
    return false;
  }
  return setting.valuePtr == &CrossPointSettings::lineSpacingPercent ||
         setting.valuePtr == &CrossPointSettings::screenMarginHorizontal ||
         setting.valuePtr == &CrossPointSettings::screenMarginTop ||
         setting.valuePtr == &CrossPointSettings::screenMarginBottom;
}

void SettingsActivity::startFontSizeEdit() {
  fontSizeEditMode = true;
  fontSizeEditDraftIndex = CrossPointSettings::fontSizeToDisplayIndex(SETTINGS.fontFamily, SETTINGS.fontSize);
}

void SettingsActivity::adjustFontSizeEdit(const int delta) {
  const int optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
  const int next = static_cast<int>(fontSizeEditDraftIndex) + delta;
  fontSizeEditDraftIndex = static_cast<uint8_t>(std::clamp(next, 0, std::max(0, optionCount - 1)));
}

void SettingsActivity::applyFontSizeEdit() {
  SETTINGS.fontSize = CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, fontSizeEditDraftIndex);
  fontSizeEditMode = false;
  persistSettingsWithLog("settings font size");
}

void SettingsActivity::startValueEdit(const SettingInfo& setting, const int categoryIndex, const int settingIndex) {
  valueEditMode = true;
  valueEditCategoryIndex = categoryIndex;
  valueEditSettingIndex = settingIndex;
  valueEditMin = setting.valueRange.min;
  valueEditMax = setting.valueRange.max;
  valueEditOriginal = SETTINGS.*(setting.valuePtr);
  valueEditDraft = std::clamp(valueEditOriginal, valueEditMin, valueEditMax);
  valueEditLastUpTapMs = 0;
  valueEditLastDownTapMs = 0;
}

void SettingsActivity::adjustValueEdit(const int delta) {
  const int next = static_cast<int>(valueEditDraft) + delta;
  valueEditDraft =
      static_cast<uint8_t>(std::clamp(next, static_cast<int>(valueEditMin), static_cast<int>(valueEditMax)));
}

void SettingsActivity::applyValueEdit() {
  if (!valueEditMode) {
    return;
  }
  const auto* settings = settingsForCategory(valueEditCategoryIndex);
  if (!settings || valueEditSettingIndex < 0 || valueEditSettingIndex >= static_cast<int>(settings->size())) {
    cancelValueEdit();
    return;
  }

  const auto& setting = (*settings)[valueEditSettingIndex];
  SETTINGS.*(setting.valuePtr) = valueEditDraft;
  if (SETTINGS.uniformMargins && setting.valuePtr == &CrossPointSettings::screenMarginHorizontal) {
    SETTINGS.screenMarginTop = valueEditDraft;
    SETTINGS.screenMarginBottom = valueEditDraft;
  }

  persistSettingsWithLog("settings value confirm");
  valueEditMode = false;
  valueEditCategoryIndex = -1;
  valueEditSettingIndex = -1;
}

void SettingsActivity::cancelValueEdit() {
  valueEditMode = false;
  valueEditCategoryIndex = -1;
  valueEditSettingIndex = -1;
}

std::string SettingsActivity::currentValueEditText() const {
  if (!valueEditMode) {
    return {};
  }
  const auto* settings = settingsForCategory(valueEditCategoryIndex);
  if (!settings || valueEditSettingIndex < 0 || valueEditSettingIndex >= static_cast<int>(settings->size())) {
    return {};
  }
  const auto& setting = (*settings)[valueEditSettingIndex];
  std::string v = std::to_string(valueEditDraft);
  if (setting.valuePtr == &CrossPointSettings::lineSpacingPercent) {
    v += "%";
  } else if (setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ) {
    v = formatUtcOffset(valueEditDraft);
  }
  return v;
}

void SettingsActivity::buildSettingsList() {
  displaySettings.clear();
  readerSettings.clear();
  statusBarSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();
  flatRows.clear();
  categoryHeaderRowIndices.clear();

  // getSettingsList() is now a const reference to a function-static cache
  // (A2). Copy each entry into its category vector — entries are then
  // mutated locally (dynamicLabels, margin filtering) without touching the
  // shared cache.
  for (const auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_STATUS_BAR) {
      statusBarSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Filter margin entries based on dynamic/uniform/separate mode.
  {
    const bool dynamic = SETTINGS.dynamicMargins;
    const bool uniform = SETTINGS.uniformMargins;
    readerSettings.erase(std::remove_if(readerSettings.begin(), readerSettings.end(),
                                        [dynamic, uniform](const SettingInfo& s) {
                                          if (dynamic) {
                                            // Dynamic: hide all manual horizontal margin controls
                                            return s.nameId == StrId::STR_UNIFORM_MARGINS ||
                                                   s.nameId == StrId::STR_SCREEN_MARGIN ||
                                                   s.nameId == StrId::STR_SCREEN_MARGIN_HORIZONTAL;
                                          } else if (uniform) {
                                            // Uniform: hide separate margin entries
                                            return s.nameId == StrId::STR_SCREEN_MARGIN_HORIZONTAL ||
                                                   s.nameId == StrId::STR_SCREEN_MARGIN_TOP ||
                                                   s.nameId == StrId::STR_SCREEN_MARGIN_BOTTOM;
                                          } else {
                                            // Separate: hide uniform margin entry
                                            return s.nameId == StrId::STR_SCREEN_MARGIN;
                                          }
                                        }),
                         readerSettings.end());
  }

  displaySettings.push_back(
      SettingInfo::Action(StrId::STR_RANDOMIZE_SLEEP_IMAGES, SettingAction::RandomizeSleepImages));
  displaySettings.push_back(SettingInfo::Action(StrId::STR_VIEW_SLEEP_WALLPAPERS, SettingAction::ViewSleepWallpapers));

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEANUP_STORAGE, SettingAction::CleanupStorage));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CHECK_UPDATES, SettingAction::CheckForUpdates));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_REFRESH_HOME_STATS, SettingAction::RefreshHomeStats));

  categoryHeaderRowIndices.resize(categoryCount, 0);
  for (int c = 0; c < categoryCount; c++) {
    categoryHeaderRowIndices[c] = static_cast<int>(flatRows.size());
    flatRows.push_back(FlatSettingRow{.isHeader = true, .categoryIndex = c, .settingIndex = -1});
    const auto* settings = settingsForCategory(c);
    for (size_t i = 0; i < settings->size(); i++) {
      flatRows.push_back(FlatSettingRow{.isHeader = false, .categoryIndex = c, .settingIndex = static_cast<int>(i)});
    }
  }
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  buildSettingsList();
  selectedRowIndex = findNextEditableRow(0, +1);
  lastNextTapMs = 0;
  lastPreviousTapMs = 0;

  renderer.requestHalfRefresh();
  requestUpdate();
}

void SettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();
  cancelValueEdit();
  persistSettingsWithLog("settings exit");
}

void SettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (fontPicker.isOpen()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      fontPicker.close();  // cancel: keep the previous font
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const bool ok = crosspoint::settings::applyFontFamilyByDisplayIndex(fontPicker.selectedDisplayIndex(),
                                                                          fontPicker.builtinCount())
                          .ok;
      fontPicker.close();
      if (ok) {
        buildSettingsList();
        selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
        persistSettingsWithLog("settings font family");
      } else {
        messagePopupText = tr(STR_FONT_LOAD_FAILED);
        messagePopupOpen = true;
      }
      requestUpdate();
      return;
    }
    buttonNavigator.onNextRelease([this] {
      fontPicker.moveDown();
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      fontPicker.moveUp();
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this] {
      fontPicker.moveDown();
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this] {
      fontPicker.moveUp();
      requestUpdate();
    });
    return;
  }

  if (enumPicker.isOpen()) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      enumPicker.close();  // cancel: keep the previous value
      enumPickerCategoryIndex = -1;
      enumPickerSettingIndex = -1;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      applyEnumPickerSelection();
      return;
    }
    buttonNavigator.onNextRelease([this] {
      enumPicker.moveDown();
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      enumPicker.moveUp();
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this] {
      enumPicker.moveDown();
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this] {
      enumPicker.moveUp();
      requestUpdate();
    });
    return;
  }

  const auto dismissPopupOnAnyPress = [this](bool& popupOpen) {
    const bool anyPress = mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                          mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                          mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Right) ||
                          mappedInput.wasPressed(MappedInputManager::Button::Power);
    if (anyPress) {
      popupOpen = false;
      requestUpdate();
      return true;
    }
    return false;
  };

  if (messagePopupOpen) {
    if (dismissPopupOnAnyPress(messagePopupOpen)) {
      messagePopupText.clear();
    }
    return;
  }

  if (homeStatsScanning) {
    // "Scanning..." was rendered on previous frame. Now do the actual scan.
    BaseTheme::refreshHomeInfoStats();
    homeStatsScanning = false;
    homeStatsPopupOpen = true;
    requestUpdate();
    return;
  }

  if (homeStatsPopupOpen) {
    if (dismissPopupOnAnyPress(homeStatsPopupOpen)) {
      BaseTheme::invalidateHomeInfoStats();
      renderer.requestFullRefresh();
    }
    return;
  }

  if (randomizePopupOpen) {
    dismissPopupOnAnyPress(randomizePopupOpen);
    return;
  }

  if (fontSizeEditMode) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      fontSizeEditMode = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      applyFontSizeEdit();
      requestUpdate();
      return;
    }

    buttonNavigator.onNextRelease([this] {
      adjustFontSizeEdit(+1);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      adjustFontSizeEdit(-1);
      requestUpdate();
    });
    buttonNavigator.onNextContinuous([this] {
      adjustFontSizeEdit(+1);
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this] {
      adjustFontSizeEdit(-1);
      requestUpdate();
    });
    return;
  }

  if (valueEditMode) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelValueEdit();
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      applyValueEdit();
      requestUpdate();
      return;
    }

    // Tap: +-1. A quick second tap (within kValueEditDoubleTapMs) jumps +-10.
    buttonNavigator.onNextRelease([this] {
      const unsigned long now = millis();
      adjustValueEdit(+crosspoint::settings::valueEditTapStep(valueEditLastUpTapMs, now,
                                                              crosspoint::settings::kValueEditDoubleTapMs));
      valueEditLastUpTapMs = now;
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this] {
      const unsigned long now = millis();
      adjustValueEdit(-crosspoint::settings::valueEditTapStep(valueEditLastDownTapMs, now,
                                                              crosspoint::settings::kValueEditDoubleTapMs));
      valueEditLastDownTapMs = now;
      requestUpdate();
    });

    // Hold: steady +-1 repeat (no acceleration).
    buttonNavigator.onNextContinuous([this] {
      adjustValueEdit(+1);
      requestUpdate();
    });

    buttonNavigator.onPreviousContinuous([this] {
      adjustValueEdit(-1);
      requestUpdate();
    });
    return;
  }

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    persistSettingsWithLog("settings back");
    onGoHome();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    const unsigned long now = millis();
    if (lastNextTapMs > 0 && now - lastNextTapMs <= doubleTapMs) {
      jumpCategory(+1);
      lastNextTapMs = 0;
    } else {
      selectedRowIndex = findNextEditableRow(selectedRowIndex, +1);
      lastNextTapMs = now;
    }
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    const unsigned long now = millis();
    if (lastPreviousTapMs > 0 && now - lastPreviousTapMs <= doubleTapMs) {
      jumpCategory(-1);
      lastPreviousTapMs = 0;
    } else {
      selectedRowIndex = findNextEditableRow(selectedRowIndex, -1);
      lastPreviousTapMs = now;
    }
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedRowIndex = findNextEditableRow(selectedRowIndex, +1);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedRowIndex = findNextEditableRow(selectedRowIndex, -1);
    requestUpdate();
  });
}

void SettingsActivity::toggleCurrentSetting() {
  if (selectedRowIndex < 0 || selectedRowIndex >= static_cast<int>(flatRows.size())) {
    return;
  }
  const auto& row = flatRows[selectedRowIndex];
  if (row.isHeader || row.settingIndex < 0) {
    return;
  }
  const auto* settings = settingsForCategory(row.categoryIndex);
  if (!settings || row.settingIndex >= static_cast<int>(settings->size())) {
    return;
  }

  const auto& setting = (*settings)[row.settingIndex];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    const bool currentValue = SETTINGS.*(setting.valuePtr);
    SETTINGS.*(setting.valuePtr) = !currentValue;
    if (setting.valuePtr == &CrossPointSettings::uniformMargins) {
      if (SETTINGS.uniformMargins) {
        SETTINGS.screenMarginTop = SETTINGS.screenMarginHorizontal;
        SETTINGS.screenMarginBottom = SETTINGS.screenMarginHorizontal;
      }
      buildSettingsList();
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    if (setting.valuePtr == &CrossPointSettings::fontSize) {
      startFontSizeEdit();
      return;
    } else if (setting.valuePtr == &CrossPointSettings::fontFamily) {
      // Open the modal vertical-list font picker. The chosen row is applied in
      // loop() when the user confirms; opening alone changes nothing.
      fontPicker.open();
      requestUpdate();
      return;
    } else if (setting.valuePtr == &CrossPointSettings::uiLanguage) {
      const uint8_t count = getLanguageCount();
      SETTINGS.uiLanguage = count > 0 ? static_cast<uint8_t>((currentValue + 1) % count) : 0;
      I18N.setLanguage(static_cast<Language>(SETTINGS.uiLanguage));
      buildSettingsList();
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    } else if (setting.enumValues.size() + setting.dynamicLabels.size() > 2) {
      // More than two options: pick from a list instead of cycling through every
      // value. The choice is applied in loop() on confirm; opening changes
      // nothing. Two-option enums fall through and keep single-click cycling.
      openEnumPicker(setting, row.categoryIndex, row.settingIndex);
      return;
    } else {
      SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    }
    if (setting.valuePtr == &CrossPointSettings::dynamicMargins) {
      buildSettingsList();
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (isPopupValueSetting(setting)) {
      startValueEdit(setting, row.categoryIndex, row.settingIndex);
      return;
    }
    const int currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue < setting.valueRange.min || currentValue > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else if (setting.type == SettingType::ACTION) {
    auto enterSubActivity = [this](Activity* activity) {
      exitActivity();
      enterNewActivity(activity);
    };

    auto onComplete = [this] {
      exitActivity();
      requestUpdate();
    };

    auto onCompleteBool = [this](bool) {
      exitActivity();
      requestUpdate();
    };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        enterSubActivity(new (std::nothrow) ButtonRemapActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::KOReaderSync:
        enterSubActivity(new (std::nothrow) KOReaderSettingsActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::OPDSBrowser:
        enterSubActivity(new (std::nothrow) CalibreSettingsActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::Network:
        enterSubActivity(new (std::nothrow) WifiSelectionActivity(renderer, mappedInput, onCompleteBool, false));
        break;
      case SettingAction::ClearCache:
        enterSubActivity(new (std::nothrow) ClearCacheActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::CleanupStorage:
        enterSubActivity(new (std::nothrow) CleanupStorageActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::CheckForUpdates:
        enterSubActivity(new (std::nothrow) OtaUpdateActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::RandomizeSleepImages:
        randomizePopupSuccess = SleepActivity::randomizeSleepImagePlaylist();
        randomizePopupOpen = true;
        requestUpdate();
        break;
      case SettingAction::ViewSleepWallpapers:
        enterSubActivity(new (std::nothrow) SleepWallpaperListActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::RefreshHomeStats: {
        homeStatsScanning = true;
        requestUpdate();
        break;
      }
      case SettingAction::None:
        // Do nothing
        break;
    }
  } else {
    return;
  }

  persistSettingsWithLog("settings toggle");
}

void SettingsActivity::openEnumPicker(const SettingInfo& setting, const int categoryIndex, const int settingIndex) {
  std::vector<std::string> labels;
  labels.reserve(setting.enumValues.size() + setting.dynamicLabels.size());
  for (const StrId id : setting.enumValues) {
    labels.emplace_back(I18N.get(id));
  }
  for (const auto& label : setting.dynamicLabels) {
    labels.push_back(label);
  }
  const int current = setting.valuePtr != nullptr ? static_cast<int>(SETTINGS.*(setting.valuePtr)) : 0;
  enumPickerCategoryIndex = categoryIndex;
  enumPickerSettingIndex = settingIndex;
  enumPicker.open(setting.nameId, std::move(labels), current);
  requestUpdate();
}

void SettingsActivity::applyEnumPickerSelection() {
  // Re-fetch the setting by stored index — the list may have been rebuilt, so a
  // reference captured at open() time could dangle.
  const auto* settings = settingsForCategory(enumPickerCategoryIndex);
  if (settings && enumPickerSettingIndex >= 0 && enumPickerSettingIndex < static_cast<int>(settings->size())) {
    const auto& setting = (*settings)[enumPickerSettingIndex];
    const int idx = enumPicker.selectedIndex();
    const int count = static_cast<int>(setting.enumValues.size() + setting.dynamicLabels.size());
    if (setting.valuePtr != nullptr && idx >= 0 && idx < count) {
      SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(idx);
    }
  }
  enumPicker.close();
  enumPickerCategoryIndex = -1;
  enumPickerSettingIndex = -1;
  // Rebuild in case the changed setting restructures the list (mirrors the
  // font-family confirm path), clamp the cursor, then persist.
  buildSettingsList();
  selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
  persistSettingsWithLog("settings enum picker");
  requestUpdate();
}

void SettingsActivity::render(Activity::RenderLock&&) {
  TransitionFeedback::resetStacking();
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  auto metrics = BaseMetrics::values;

  // Top status line: version left, battery right
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, metrics.topPadding + 5, CROSSPOINT_VERSION);
  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = pageWidth - 12;
  GUI.drawBatteryRight(renderer, Rect{batteryX, metrics.topPadding + 5, metrics.batteryWidth, metrics.batteryHeight},
                       showBatteryPercentage, UI_10_FONT_ID, metrics.batteryWidth, metrics.batteryHeight, false);

  const int contentY = metrics.topPadding + metrics.headerHeight;
  const int contentHeight = pageHeight - (contentY + metrics.buttonHintsHeight + metrics.verticalSpacing);
  const int rowHeight = metrics.listRowHeight;
  const int rowFont = UI_10_FONT_ID;
  constexpr int kChipPad = 1;

  // Compute a row's value text (empty string if the row has no value column).
  auto computeValueText = [&](const FlatSettingRow& row) -> std::string {
    if (row.isHeader) {
      return std::string();
    }
    const auto* settings = settingsForCategory(row.categoryIndex);
    const auto& setting = (*settings)[row.settingIndex];
    if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
      return (SETTINGS.*(setting.valuePtr)) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    }
    if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
      if (setting.valuePtr == &CrossPointSettings::fontSize) {
        return fontSizeValueLabel(SETTINGS.fontFamily, SETTINGS.fontSize);
      }
      if (setting.valuePtr == &CrossPointSettings::fontFamily) {
        return I18N.get(setting.enumValues[CrossPointSettings::fontFamilyToDisplayIndex(SETTINGS.fontFamily)]);
      }
      if (setting.valuePtr == &CrossPointSettings::uiLanguage) {
        // Language names are self-descriptive ("English", "Español",
        // "Slovenščina") — render directly from the generated table rather
        // than translating a StrId.
        const uint8_t idx = SETTINGS.uiLanguage < getLanguageCount() ? SETTINGS.uiLanguage : 0;
        return LANGUAGE_NAMES[idx];
      }
      return I18N.get(setting.enumValues[SETTINGS.*(setting.valuePtr)]);
    }
    if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
      const uint8_t valueToShow =
          (valueEditMode && row.categoryIndex == valueEditCategoryIndex && row.settingIndex == valueEditSettingIndex)
              ? valueEditDraft
              : SETTINGS.*(setting.valuePtr);
      std::string valueText = std::to_string(valueToShow);
      if (setting.valuePtr == &CrossPointSettings::lineSpacingPercent) {
        valueText += "%";
      } else if (setting.valuePtr == &CrossPointSettings::clockUtcOffsetQ) {
        valueText = formatUtcOffset(valueToShow);
      }
      return valueText;
    }
    return std::string();
  };

  const int availableWidth = pageWidth - metrics.contentSidePadding * 2;
  const int wrapGap = metrics.contentSidePadding;

  // Take the longest UTF-8-safe prefix of `word` that still fits in `maxW`.
  // Returns the prefix and the remainder.
  auto takePrefixFit = [&](const std::string& word, int maxW) -> std::pair<std::string, std::string> {
    int bestEnd = 0;
    for (int end = 1; end <= static_cast<int>(word.size()); end++) {
      // Skip UTF-8 continuation bytes so we never split a codepoint.
      if (end < static_cast<int>(word.size()) && (static_cast<uint8_t>(word[end]) & 0xC0) == 0x80) {
        continue;
      }
      const std::string candidate = word.substr(0, end);
      if (renderer.getTextWidth(rowFont, candidate.c_str()) <= maxW) {
        bestEnd = end;
      } else {
        break;
      }
    }
    if (bestEnd == 0) {
      // availableWidth too small to fit even one codepoint — draw nothing
      // rather than spin forever. Should be impossible on real screens.
      return {std::string(), word};
    }
    return {word.substr(0, bestEnd), word.substr(bestEnd)};
  };

  // Split a label into up to two lines, greedy-pack by word. If a single
  // word exceeds availableWidth, force-break it mid-codepoint.
  auto splitLabel = [&](const char* label) -> std::pair<std::string, std::string> {
    std::string s(label);
    if (renderer.getTextWidth(rowFont, s.c_str()) <= availableWidth) {
      return {s, std::string()};
    }
    std::vector<std::string> words;
    {
      std::string cur;
      for (char c : s) {
        if (c == ' ') {
          if (!cur.empty()) {
            words.push_back(std::move(cur));
            cur.clear();
          }
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) {
        words.push_back(std::move(cur));
      }
    }

    std::string line1;
    size_t wi = 0;
    while (wi < words.size()) {
      const std::string candidate = line1.empty() ? words[wi] : (line1 + " " + words[wi]);
      if (renderer.getTextWidth(rowFont, candidate.c_str()) <= availableWidth) {
        line1 = candidate;
        wi++;
      } else {
        break;
      }
    }
    if (line1.empty() && wi < words.size()) {
      auto split = takePrefixFit(words[wi], availableWidth);
      line1 = split.first;
      if (!split.second.empty()) {
        words[wi] = split.second;
      } else {
        wi++;
      }
    }

    std::string line2;
    while (wi < words.size()) {
      const std::string candidate = line2.empty() ? words[wi] : (line2 + " " + words[wi]);
      if (renderer.getTextWidth(rowFont, candidate.c_str()) <= availableWidth) {
        line2 = candidate;
        wi++;
      } else {
        if (line2.empty()) {
          auto split = takePrefixFit(words[wi], availableWidth);
          line2 = split.first;
        }
        break;
      }
    }
    return {line1, line2};
  };

  // Pre-scan rows: compute value text, decide how label wraps, and pack
  // each entry into 1/2/3-row slots so later pagination never clips text.
  const int rowCount = static_cast<int>(flatRows.size());
  std::vector<std::string> valueTexts(rowCount);
  std::vector<std::string> labelLine1s(rowCount);
  std::vector<std::string> labelLine2s(rowCount);
  std::vector<uint8_t> valueLineOffset(rowCount, 0);
  std::vector<int> rowHeights(rowCount, rowHeight);

  for (int i = 0; i < rowCount; i++) {
    const auto& row = flatRows[i];
    if (row.isHeader) {
      continue;
    }
    const auto* settings = settingsForCategory(row.categoryIndex);
    const auto& setting = (*settings)[row.settingIndex];
    const char* settingName = I18N.get(setting.nameId);
    auto labelSplit = splitLabel(settingName);
    labelLine1s[i] = std::move(labelSplit.first);
    labelLine2s[i] = std::move(labelSplit.second);
    const int labelLineCount = labelLine2s[i].empty() ? 1 : 2;

    std::string v = computeValueText(row);
    if (v.empty()) {
      rowHeights[i] = labelLineCount * rowHeight;
      continue;
    }

    const int valueW = renderer.getTextWidth(rowFont, v.c_str());
    const int line1W = renderer.getTextWidth(rowFont, labelLine1s[i].c_str());
    const bool canInline = (labelLineCount == 1) && (line1W + wrapGap + valueW <= availableWidth);
    if (canInline) {
      rowHeights[i] = rowHeight;
      valueLineOffset[i] = 0;
    } else {
      rowHeights[i] = (labelLineCount + 1) * rowHeight;
      valueLineOffset[i] = static_cast<uint8_t>(labelLineCount);
    }
    valueTexts[i] = std::move(v);
  }

  // Greedy page packing: each page holds as many rows as fit in contentHeight.
  std::vector<int> pageOfRow(rowCount, 0);
  {
    int curPage = 0;
    int usedH = 0;
    for (int i = 0; i < rowCount; i++) {
      if (usedH > 0 && usedH + rowHeights[i] > contentHeight) {
        curPage++;
        usedH = 0;
      }
      pageOfRow[i] = curPage;
      usedH += rowHeights[i];
    }
  }

  int pageStartIndex = 0;
  if (rowCount > 0) {
    const int clampedSel = std::min(std::max(selectedRowIndex, 0), rowCount - 1);
    const int targetPage = pageOfRow[clampedSel];
    while (pageStartIndex < rowCount && pageOfRow[pageStartIndex] != targetPage) {
      pageStartIndex++;
    }
  }

  const int renderPage = (rowCount > 0) ? pageOfRow[pageStartIndex] : 0;
  int rowYOffset = 0;
  for (int i = pageStartIndex; i < rowCount && pageOfRow[i] == renderPage; i++) {
    const int rowY = contentY + rowYOffset;
    const auto& row = flatRows[i];
    const int thisRowHeight = rowHeights[i];

    if (row.isHeader) {
      renderer.fillRect(0, rowY, pageWidth, rowHeight, true);
      const char* label = I18N.get(categoryNames[row.categoryIndex]);
      const int textW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
      const int textX = (pageWidth - textW) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, rowY, label, false, EpdFontFamily::REGULAR);
      rowYOffset += thisRowHeight;
      continue;
    }

    const auto* settings = settingsForCategory(row.categoryIndex);
    const auto& setting = (*settings)[row.settingIndex];
    const bool isSelected = (i == selectedRowIndex);
    const int textH = renderer.getTextHeight(rowFont);
    const int chipH = textH + kChipPad * 2;

    const std::string& labelL1 = labelLine1s[i];
    const std::string& labelL2 = labelLine2s[i];
    const bool hasLabelL2 = !labelL2.empty();
    const int labelL1Y = rowY;
    const int labelL2Y = rowY + rowHeight;
    const int valueY = rowY + valueLineOffset[i] * rowHeight;
    const int labelL1ChipY = labelL1Y + (rowHeight - chipH) / 2;
    const int labelL2ChipY = labelL2Y + (rowHeight - chipH) / 2;
    const int valueChipY = valueY + (rowHeight - chipH) / 2;

    const int labelL1W = renderer.getTextWidth(rowFont, labelL1.c_str());
    if (isSelected) {
      renderer.fillRect(metrics.contentSidePadding - kChipPad, labelL1ChipY, labelL1W + kChipPad * 2, chipH, true);
    }
    renderer.drawText(rowFont, metrics.contentSidePadding, labelL1Y, labelL1.c_str(), !isSelected);

    if (hasLabelL2) {
      const int labelL2W = renderer.getTextWidth(rowFont, labelL2.c_str());
      if (isSelected) {
        renderer.fillRect(metrics.contentSidePadding - kChipPad, labelL2ChipY, labelL2W + kChipPad * 2, chipH, true);
      }
      renderer.drawText(rowFont, metrics.contentSidePadding, labelL2Y, labelL2.c_str(), !isSelected);
    }

    const std::string& valueText = valueTexts[i];
    if (!valueText.empty()) {
      const int valueW = renderer.getTextWidth(rowFont, valueText.c_str());
      const int valueX = pageWidth - metrics.contentSidePadding - valueW;
      if (isSelected) {
        renderer.fillRect(valueX - kChipPad, valueChipY, valueW + kChipPad * 2, chipH, true);
      }
      renderer.drawText(rowFont, valueX, valueY, valueText.c_str(), !isSelected);
    }

    rowYOffset += thisRowHeight;
  }

  // Draw help text
  const char* confirmLabel = (fontPicker.isOpen() || enumPicker.isOpen()) ? tr(STR_SELECT)
                             : (fontSizeEditMode || valueEditMode)        ? tr(STR_CONFIRM)
                                                                          : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (homeStatsScanning) {
    GUI.drawPopup(renderer, tr(STR_SCANNING_SD_CARD));
  }

  if (homeStatsPopupOpen) {
    GUI.drawHomeInfoStatsPopup(renderer);
  }

  if (randomizePopupOpen) {
    const char* msg = randomizePopupSuccess ? tr(STR_DONE) : tr(STR_NO_ENTRIES);
    GUI.drawPopup(renderer, msg);
  }

  if (messagePopupOpen) {
    GUI.drawPopup(renderer, messagePopupText.c_str());
  }

  if (fontSizeEditMode) {
    const int optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
    const int textH = renderer.getTextHeight(UI_12_FONT_ID);
    constexpr int kItemPadH = 4;
    constexpr int kItemPadV = 3;
    constexpr int kItemGap = 10;
    constexpr int kPopupPad = 16;

    auto labelAt = [&](int i) -> std::string {
      const uint8_t fs = CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, i);
      return fontSizeValueLabel(SETTINGS.fontFamily, fs);
    };

    // Pre-measure each chip's width — reused for both the row-fitting
    // pass and the draw pass.
    std::vector<int> chipWidths(optionCount);
    int singleRowW = 0;
    for (int i = 0; i < optionCount; i++) {
      const int labelW = renderer.getTextWidth(UI_12_FONT_ID, labelAt(i).c_str());
      chipWidths[i] = labelW + kItemPadH * 2;
      singleRowW += chipWidths[i];
    }
    singleRowW += kItemGap * std::max(0, optionCount - 1);

    const char* title = tr(STR_FONT_SIZE);
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, title);

    // Decide popup width first, then wrap chips into rows that fit
    // inside that width minus padding. Custom families can install up
    // to 16 sizes (25..40) so the single-row layout previously ran the
    // chips off both edges of the screen at small font sizes — the
    // popupW was capped to the screen but curX kept advancing past it.
    const int popupW = std::min(pageWidth - 20, std::max(singleRowW, titleW) + kPopupPad * 2);
    const int rowMaxW = popupW - kPopupPad * 2;

    // Pack chips into rows that fit rowMaxW, with a hard cap of
    // kMaxChipsPerRow per row so the popup stays readable on narrow
    // screens even when individual chips are tiny.
    constexpr int kMaxChipsPerRow = 8;
    std::vector<int> rowStarts;  // chip index where each row begins
    std::vector<int> rowWidths;  // pixel width occupied by each row
    rowStarts.push_back(0);
    int curRowW = 0;
    int curRowChips = 0;
    for (int i = 0; i < optionCount; i++) {
      const int addW = (curRowW == 0) ? chipWidths[i] : (kItemGap + chipWidths[i]);
      const bool overflow = (curRowW != 0 && curRowW + addW > rowMaxW);
      const bool capReached = (curRowChips >= kMaxChipsPerRow);
      if (overflow || capReached) {
        rowWidths.push_back(curRowW);
        rowStarts.push_back(i);
        curRowW = chipWidths[i];
        curRowChips = 1;
      } else {
        curRowW += addW;
        curRowChips++;
      }
    }
    rowWidths.push_back(curRowW);

    constexpr int kRowGap = 4;
    const int chipsBlockH = static_cast<int>(rowWidths.size()) * (textH + kItemPadV * 2) +
                            std::max(0, static_cast<int>(rowWidths.size()) - 1) * kRowGap;
    constexpr int kTitleH = 16;
    constexpr int kTitleToChipsGap = 6;
    const int popupH = kPopupPad + kTitleH + kTitleToChipsGap + chipsBlockH + kPopupPad;
    const int popupX = (pageWidth - popupW) / 2;
    const int popupY = (pageHeight - popupH) / 2;

    renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
    renderer.fillRect(popupX, popupY, popupW, popupH, false);
    renderer.drawText(UI_10_FONT_ID, popupX + (popupW - titleW) / 2, popupY + kPopupPad - 10, title, true);

    int rowY = popupY + kPopupPad + kTitleH + kTitleToChipsGap;
    for (size_t r = 0; r < rowWidths.size(); r++) {
      const int rowStart = rowStarts[r];
      const int rowEnd = (r + 1 < rowStarts.size()) ? rowStarts[r + 1] : optionCount;
      int curX = popupX + (popupW - rowWidths[r]) / 2;
      for (int i = rowStart; i < rowEnd; i++) {
        const std::string label = labelAt(i);
        const int chipW = chipWidths[i];
        const bool isSelected = (i == static_cast<int>(fontSizeEditDraftIndex));
        if (isSelected) {
          renderer.fillRect(curX, rowY, chipW, textH + kItemPadV * 2, true);
        }
        renderer.drawText(UI_12_FONT_ID, curX + kItemPadH, rowY + kItemPadV, label.c_str(), !isSelected);
        curX += chipW + kItemGap;
      }
      rowY += textH + kItemPadV * 2 + kRowGap;
    }
  }

  if (valueEditMode) {
    const auto* settings = settingsForCategory(valueEditCategoryIndex);
    if (settings && valueEditSettingIndex >= 0 && valueEditSettingIndex < static_cast<int>(settings->size())) {
      const auto& setting = (*settings)[valueEditSettingIndex];
      const char* settingLabel = I18N.get(setting.nameId);
      const std::string valueText = currentValueEditText();

      const int popupW = std::min(pageWidth - 30, 300);
      const int popupH = 86;
      const int popupX = (pageWidth - popupW) / 2;
      const int popupY = (pageHeight - popupH) / 2;

      renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
      renderer.fillRect(popupX, popupY, popupW, popupH, false);

      const int titleW = renderer.getTextWidth(UI_10_FONT_ID, settingLabel);
      renderer.drawText(UI_10_FONT_ID, popupX + (popupW - titleW) / 2, popupY + 8, settingLabel, true);

      const int valueW = renderer.getTextWidth(UI_12_FONT_ID, valueText.c_str());
      renderer.drawText(UI_12_FONT_ID, popupX + (popupW - valueW) / 2, popupY + 30, valueText.c_str(), true);

      const int barX = popupX + 20;
      const int barY = popupY + popupH - 22;
      const int barW = popupW - 40;
      const int barH = 8;
      renderer.drawRect(barX, barY, barW, barH, true);
      const int range = std::max(1, static_cast<int>(valueEditMax) - static_cast<int>(valueEditMin));
      const int filledW =
          2 + ((static_cast<int>(valueEditDraft) - static_cast<int>(valueEditMin)) * std::max(1, barW - 4)) / range;
      renderer.fillRect(barX + 2, barY + 2, filledW, std::max(1, barH - 4), true);
    }
  }

  if (fontPicker.isOpen()) {
    fontPicker.render(renderer, pageWidth, pageHeight);
  }
  if (enumPicker.isOpen()) {
    enumPicker.render(renderer, pageWidth, pageHeight);
  }

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
