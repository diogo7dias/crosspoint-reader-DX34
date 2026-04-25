#include "SettingsActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "BleConnectActivity.h"
#include "BleRemapActivity.h"
#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "CustomFontsSettingsActivity.h"
#include "KOReaderSettingsActivity.h"
#include "MappedInputManager.h"
#include "OtaUpdateActivity.h"
#include "SettingsList.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "fonts/CustomBinFontManager.h"
#include "util/TransitionFeedback.h"

const StrId SettingsActivity::categoryNames[categoryCount] = {StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER,
                                                              StrId::STR_STATUS_BAR, StrId::STR_CAT_CONTROLS,
                                                              StrId::STR_CAT_SYSTEM};

namespace {
constexpr unsigned long doubleTapMs = 350;

void persistSettingsWithLog(const char* context) {
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("SET", "Failed to save settings (%s)", context);
  }
}

std::string fontSizeValueLabel(const uint8_t family, const uint8_t fontSize) {
  if (family == CrossPointSettings::CUSTOM_FAMILY) {
    // Custom families don't use the small/medium/large enum — the
    // pixel size lives in customFontSizePt. Caller passes that
    // (or the live edit-draft value) via the fontSize argument.
    return std::to_string(fontSize);
  }
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
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
    // Custom families: draft index points into installedSizesFor; snap
    // to whichever slot matches the persisted customFontSizePt, or 0
    // when the family was rebuilt and the previous size disappeared.
    const auto sizes = crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName);
    fontSizeEditDraftIndex = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
      if (sizes[i] == SETTINGS.customFontSizePt) {
        fontSizeEditDraftIndex = static_cast<uint8_t>(i);
        break;
      }
    }
    return;
  }
  fontSizeEditDraftIndex = CrossPointSettings::fontSizeToDisplayIndex(SETTINGS.fontFamily, SETTINGS.fontSize);
}

void SettingsActivity::adjustFontSizeEdit(const int delta) {
  int optionCount = 0;
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
    optionCount = static_cast<int>(
        crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName).size());
    if (optionCount == 0) optionCount = 1;
  } else {
    optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
  }
  const int next = static_cast<int>(fontSizeEditDraftIndex) + delta;
  fontSizeEditDraftIndex = static_cast<uint8_t>(std::clamp(next, 0, std::max(0, optionCount - 1)));
}

void SettingsActivity::applyFontSizeEdit() {
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
    const auto sizes = crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName);
    if (!sizes.empty() && fontSizeEditDraftIndex < sizes.size()) {
      const uint16_t trialSize = sizes[fontSizeEditDraftIndex];
      const uint16_t prevSize = SETTINGS.customFontSizePt;
      // activate() is atomic — on failure the previous active font
      // stays registered in the renderer, so the only state to roll
      // back is SETTINGS. Without this revert the persisted size moves
      // to a font the renderer never accepted, every later
      // ensureSectionLoaded re-attempts the failing activate, and
      // fontId mismatches invalidate the section cache on each chapter.
      if (!crosspoint::fonts::CustomBinFontManager::instance().activate(SETTINGS.customFontName, trialSize)) {
        SETTINGS.customFontSizePt = prevSize;
        messagePopupText = tr(STR_FONT_LOAD_FAILED);
        messagePopupOpen = true;
        fontSizeEditMode = false;
        requestUpdate();
        return;
      }
      SETTINGS.customFontSizePt = trialSize;
    }
  } else {
    SETTINGS.fontSize = CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, fontSizeEditDraftIndex);
  }
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
}

void SettingsActivity::adjustValueEdit(const int delta) {
  const int next = static_cast<int>(valueEditDraft) + delta;
  valueEditDraft =
      static_cast<uint8_t>(std::clamp(next, static_cast<int>(valueEditMin), static_cast<int>(valueEditMax)));
}

namespace {
int getValueEditHoldStep(const MappedInputManager& mappedInput, const SettingInfo&) {
  return mappedInput.getHeldTime() >= 3000 ? 10 : 1;
}
}  // namespace

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

  for (auto& setting : getSettingsList()) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_STATUS_BAR) {
      statusBarSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(std::move(setting));
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(std::move(setting));
    }
    // Web-only categories (KOReader Sync, OPDS Browser) are skipped for device UI
  }

  // Append every installed custom font family as a dynamic label on
  // the fontFamily picker. The cycle handler maps display indices
  // [enumValues.size(), enumValues.size()+dynamicLabels.size()) onto
  // SETTINGS.customFontName so the global picker reaches the same
  // families the in-book picker already does.
  for (auto& s : readerSettings) {
    if (s.valuePtr != &CrossPointSettings::fontFamily) continue;
    s.dynamicLabels.clear();
    const auto names = crosspoint::fonts::CustomBinFontManager::instance().familyNames();
    s.dynamicLabels.reserve(names.size());
    for (const auto& name : names) {
      s.dynamicLabels.push_back(std::string(I18N.get(StrId::STR_CUSTOM_PREFIX)) + name);
    }
    break;
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

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  controlsSettings.push_back(SettingInfo::Action(StrId::STR_BLUETOOTH_HID, SettingAction::BluetoothHID));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_KOREADER_SYNC, SettingAction::KOReaderSync));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_OPDS_BROWSER, SettingAction::OPDSBrowser));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_MANAGE_CUSTOM_FONTS, SettingAction::ManageCustomFonts));
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

    buttonNavigator.onNextRelease([this] {
      adjustValueEdit(+1);
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this] {
      adjustValueEdit(-1);
      requestUpdate();
    });

    buttonNavigator.onNextContinuous([this] {
      const auto* settings = settingsForCategory(valueEditCategoryIndex);
      if (!settings || valueEditSettingIndex < 0 || valueEditSettingIndex >= static_cast<int>(settings->size())) {
        return;
      }
      adjustValueEdit(+getValueEditHoldStep(mappedInput, (*settings)[valueEditSettingIndex]));
      requestUpdate();
    });

    buttonNavigator.onPreviousContinuous([this] {
      const auto* settings = settingsForCategory(valueEditCategoryIndex);
      if (!settings || valueEditSettingIndex < 0 || valueEditSettingIndex >= static_cast<int>(settings->size())) {
        return;
      }
      adjustValueEdit(-getValueEditHoldStep(mappedInput, (*settings)[valueEditSettingIndex]));
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
      // Picker: 5 built-in StrIds + N installed custom families. Cycle
      // resolves against the sum. Custom slots set fontFamily =
      // CUSTOM_FAMILY + customFontName + a valid customFontSizePt
      // (preserving the previous choice when still installed for the
      // new family — see TODO #77 for the in-book parity).
      const auto names = crosspoint::fonts::CustomBinFontManager::instance().familyNames();
      const size_t builtinCount = setting.enumValues.size();
      const size_t customCount = names.size();
      const size_t total = builtinCount + customCount;
      size_t currentIndex = CrossPointSettings::fontFamilyToDisplayIndex(SETTINGS.fontFamily);
      if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
        currentIndex = builtinCount;
        for (size_t i = 0; i < names.size(); ++i) {
          if (names[i] == SETTINGS.customFontName) {
            currentIndex = builtinCount + i;
            break;
          }
        }
      }
      const size_t nextIndex = total == 0 ? 0 : (currentIndex + 1) % total;

      // Snapshot before any mutation so we can revert atomically if the
      // new family is a custom one whose tables don't fit the per-variant
      // budget — without this revert, the persisted SETTINGS point at a
      // font the renderer never accepted and ensureSectionLoaded re-tries
      // the failing activate on every chapter.
      const auto prevFamily = SETTINGS.fontFamily;
      const auto prevName = SETTINGS.customFontName;
      const auto prevSize = SETTINGS.customFontSizePt;

      if (nextIndex < builtinCount) {
        SETTINGS.fontFamily = CrossPointSettings::displayIndexToFontFamily(static_cast<uint8_t>(nextIndex));
        SETTINGS.customFontName.clear();
        SETTINGS.customFontSizePt = 0;
      } else {
        SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FAMILY;
        SETTINGS.customFontName = names[nextIndex - builtinCount];
        const auto sizes =
            crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName);
        bool keep = false;
        for (auto s : sizes) {
          if (s == SETTINGS.customFontSizePt) {
            keep = true;
            break;
          }
        }
        if (!keep) SETTINGS.customFontSizePt = sizes.empty() ? 0 : sizes.front();
      }
      auto& customBinFonts = crosspoint::fonts::CustomBinFontManager::instance();
      // activate() is atomic: on failure the previous active font stays
      // registered. Try first, then deactivate only when the new state
      // is a built-in family or the new custom activation succeeded.
      bool ok = true;
      if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY && !SETTINGS.customFontName.empty()) {
        ok = customBinFonts.activate(SETTINGS.customFontName, SETTINGS.customFontSizePt);
      } else {
        customBinFonts.deactivate();
      }
      if (!ok) {
        SETTINGS.fontFamily = prevFamily;
        SETTINGS.customFontName = prevName;
        SETTINGS.customFontSizePt = prevSize;
        messagePopupText = tr(STR_FONT_LOAD_FAILED);
        messagePopupOpen = true;
        requestUpdate();
        return;
      }
    } else if (setting.valuePtr == &CrossPointSettings::uiLanguage) {
      const uint8_t count = getLanguageCount();
      SETTINGS.uiLanguage = count > 0 ? static_cast<uint8_t>((currentValue + 1) % count) : 0;
      I18N.setLanguage(static_cast<Language>(SETTINGS.uiLanguage));
      buildSettingsList();
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    } else {
      SETTINGS.*(setting.valuePtr) = (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    }
    if (setting.valuePtr == &CrossPointSettings::fontFamily) {
      SETTINGS.fontFamily = CrossPointSettings::normalizeFontFamily(SETTINGS.fontFamily);
      SETTINGS.fontSize = CrossPointSettings::normalizeFontSizeForFamily(SETTINGS.fontFamily, SETTINGS.fontSize);
      SETTINGS.lineSpacingPercent = CrossPointSettings::resetLineSpacingPercentForFamily(SETTINGS.fontFamily);
      buildSettingsList();
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
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
        enterSubActivity(new ButtonRemapActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::BluetoothHID: {
        // Temporarily disabled: BLE HID path is mid-rework for issue #44 and
        // must not be entered from the UI until the Gamebrick decoder lands
        // and a volunteer has verified pairing on real hardware. The row is
        // still visible with a strikethrough so users see the feature is
        // upcoming; confirm presses are ignored here.
        break;
      }
      case SettingAction::KOReaderSync:
        enterSubActivity(new KOReaderSettingsActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::OPDSBrowser:
        enterSubActivity(new CalibreSettingsActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::Network:
        enterSubActivity(new WifiSelectionActivity(renderer, mappedInput, onCompleteBool, false));
        break;
      case SettingAction::ClearCache:
        enterSubActivity(new ClearCacheActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::ManageCustomFonts:
        enterSubActivity(new CustomFontsSettingsActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::CheckForUpdates:
        enterSubActivity(new OtaUpdateActivity(renderer, mappedInput, onComplete));
        break;
      case SettingAction::RandomizeSleepImages:
        randomizePopupSuccess = SleepActivity::randomizeSleepImagePlaylist();
        randomizePopupOpen = true;
        requestUpdate();
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
        const uint8_t size =
            (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) ? SETTINGS.customFontSizePt : SETTINGS.fontSize;
        return fontSizeValueLabel(SETTINGS.fontFamily, size);
      }
      if (setting.valuePtr == &CrossPointSettings::fontFamily) {
        if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
          const auto names = crosspoint::fonts::CustomBinFontManager::instance().familyNames();
          for (size_t k = 0; k < names.size(); ++k) {
            if (names[k] == SETTINGS.customFontName && k < setting.dynamicLabels.size()) {
              return setting.dynamicLabels[k];
            }
          }
          // Fallback when the configured custom name no longer exists
          // (file deleted off SD): show the first installed family or
          // ChareInk if none are installed.
          if (!setting.dynamicLabels.empty()) return setting.dynamicLabels[0];
          return I18N.get(setting.enumValues[0]);
        }
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

    const bool isDisabledAction =
        (setting.type == SettingType::ACTION && setting.action == SettingAction::BluetoothHID);

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

    if (isDisabledAction) {
      // Strike-through indicates "upcoming but disabled for now". Inverted
      // foreground color when the row is selected (highlight draws the
      // background black, so the line must be drawn white to remain visible).
      // 2px thickness — two stacked 1px lines — so the mark reads clearly
      // against the row font at a glance.
      const int strikeY = labelL1Y + textH / 2;
      renderer.drawLine(metrics.contentSidePadding, strikeY, metrics.contentSidePadding + labelL1W, strikeY,
                        !isSelected);
      renderer.drawLine(metrics.contentSidePadding, strikeY + 1, metrics.contentSidePadding + labelL1W, strikeY + 1,
                        !isSelected);
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
  const char* confirmLabel = (fontSizeEditMode || valueEditMode) ? tr(STR_CONFIRM) : tr(STR_TOGGLE);
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
    // Build the option list once for both the layout pass and the
    // draw pass — avoids walking installedSizesFor twice and keeps
    // the built-in path identical to before.
    const bool customFamily = (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY);
    std::vector<uint8_t> customSizes;
    int optionCount = 0;
    if (customFamily) {
      customSizes = crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName);
      optionCount = static_cast<int>(customSizes.size());
    } else {
      optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
    }
    const int textH = renderer.getTextHeight(UI_12_FONT_ID);
    constexpr int kItemPadH = 4;
    constexpr int kItemPadV = 3;
    constexpr int kItemGap = 10;
    constexpr int kPopupPad = 16;

    auto labelAt = [&](int i) -> std::string {
      if (customFamily) {
        return std::to_string(customSizes[i]);
      }
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

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
