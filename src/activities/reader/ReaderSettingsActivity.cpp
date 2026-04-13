#include "ReaderSettingsActivity.h"

#include <algorithm>

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingThemeStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
const StrId kCategoryNames[] = {StrId::STR_CAT_READER, StrId::STR_STATUS_BAR};

std::string fontSizeValueLabel(const uint8_t family, const uint8_t fontSize) {
  return std::to_string(
      CrossPointSettings::fontSizeToPointSize(family, fontSize));
}

int getValueEditHoldStep(const MappedInputManager& mappedInput,
                         const SettingInfo&) {
  return mappedInput.getHeldTime() >= 1200 ? 5 : 1;
}

int readerFontIdFor(const uint8_t family, const uint8_t fontSize) {
  const uint8_t normalizedFontSize =
      CrossPointSettings::normalizeFontSizeForFamily(family, fontSize);

  if (CrossPointSettings::normalizeFontFamily(family) ==
      CrossPointSettings::BOOKERLY) {
    switch (normalizedFontSize) {
    case CrossPointSettings::SIZE_13:
      return BOOKERLY_13_FONT_ID;
    case CrossPointSettings::SIZE_14:
      return BOOKERLY_14_FONT_ID;
    case CrossPointSettings::MEDIUM:
      return BOOKERLY_15_FONT_ID;
    case CrossPointSettings::SIZE_16:
      return BOOKERLY_16_FONT_ID;
    case CrossPointSettings::LARGE:
      return BOOKERLY_17_FONT_ID;
    case CrossPointSettings::SIZE_18:
    default:
      return BOOKERLY_18_FONT_ID;
    }
  }
  if (CrossPointSettings::normalizeFontFamily(family) ==
      CrossPointSettings::VOLLKORN) {
    switch (normalizedFontSize) {
    case CrossPointSettings::SIZE_13:
      return VOLLKORN_13_FONT_ID;
    case CrossPointSettings::SIZE_14:
      return VOLLKORN_14_FONT_ID;
    case CrossPointSettings::MEDIUM:
      return VOLLKORN_15_FONT_ID;
    case CrossPointSettings::SIZE_16:
      return VOLLKORN_16_FONT_ID;
    case CrossPointSettings::LARGE:
      return VOLLKORN_17_FONT_ID;
    case CrossPointSettings::SIZE_18:
    default:
      return VOLLKORN_18_FONT_ID;
    }
  }
  switch (normalizedFontSize) {
    case CrossPointSettings::SIZE_14:
      return CHAREINK_14_FONT_ID;
    case CrossPointSettings::MEDIUM:
      return CHAREINK_15_FONT_ID;
    case CrossPointSettings::SIZE_16:
      return CHAREINK_16_FONT_ID;
    case CrossPointSettings::LARGE:
      return CHAREINK_17_FONT_ID;
    case CrossPointSettings::SIZE_18:
    default:
      return CHAREINK_18_FONT_ID;
    }
}
}  // namespace

bool ReaderSettingsActivity::isTxtContext() const {
  return bookCachePath.find("/txt_") != std::string::npos;
}

void ReaderSettingsActivity::persistSettings(const char* context) {
  if (bookCachePath.empty()) {
    // No book context — save to global settings
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("RSET", "Failed to save settings (%s)", context);
      return;
    }
  } else {
    // In-book context — save per-book only, keep global as new-book defaults
    if (!READING_THEMES.saveCurrentBookSettings(bookCachePath)) {
      LOG_ERR("RSET", "Failed to save book settings (%s)", context);
      return;
    }
  }

  dirty = true;
}

const std::vector<SettingInfo>* ReaderSettingsActivity::settingsForCategory(
    const int categoryIndex) const {
  switch (categoryIndex) {
    case 0:
      return &readerSettings;
    case 1:
    default:
      return &statusBarSettings;
  }
}

int ReaderSettingsActivity::findNextEditableRow(const int startIndex,
                                                const int direction) const {
  if (flatRows.empty()) {
    return 0;
  }
  int idx = startIndex;
  for (size_t i = 0; i < flatRows.size(); i++) {
    idx = (direction > 0)
              ? ButtonNavigator::nextIndex(idx, static_cast<int>(flatRows.size()))
              : ButtonNavigator::previousIndex(
                    idx, static_cast<int>(flatRows.size()));
    if (!flatRows[idx].isHeader) {
      return idx;
    }
  }
  return startIndex;
}

bool ReaderSettingsActivity::isPopupValueSetting(
    const SettingInfo& setting) const {
  if (setting.type != SettingType::VALUE || setting.valuePtr == nullptr) {
    return false;
  }
  return setting.valuePtr == &CrossPointSettings::lineSpacingPercent ||
         setting.valuePtr == &CrossPointSettings::screenMarginHorizontal ||
         setting.valuePtr == &CrossPointSettings::screenMarginTop ||
         setting.valuePtr == &CrossPointSettings::screenMarginBottom;
}

void ReaderSettingsActivity::startFontSizeEdit() {
  fontSizeEditMode = true;
  fontSizeEditDraftIndex = CrossPointSettings::fontSizeToDisplayIndex(
      SETTINGS.fontFamily, SETTINGS.fontSize);
}

void ReaderSettingsActivity::adjustFontSizeEdit(const int delta) {
  const int optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
  const int next = static_cast<int>(fontSizeEditDraftIndex) + delta;
  fontSizeEditDraftIndex = static_cast<uint8_t>(
      std::clamp(next, 0, std::max(0, optionCount - 1)));
}

void ReaderSettingsActivity::applyFontSizeEdit() {
  SETTINGS.fontSize = CrossPointSettings::displayIndexToFontSize(
      SETTINGS.fontFamily, fontSizeEditDraftIndex);
  fontSizeEditMode = false;
  persistSettings("reader settings font size");
}

void ReaderSettingsActivity::startValueEdit(const SettingInfo& setting,
                                            const int categoryIndex,
                                            const int settingIndex) {
  valueEditMode = true;
  valueEditCategoryIndex = categoryIndex;
  valueEditSettingIndex = settingIndex;
  valueEditMin = setting.valueRange.min;
  valueEditMax = setting.valueRange.max;
  valueEditDraft =
      std::clamp(SETTINGS.*(setting.valuePtr), valueEditMin, valueEditMax);
}

void ReaderSettingsActivity::adjustValueEdit(const int delta) {
  const int next = static_cast<int>(valueEditDraft) + delta;
  valueEditDraft = static_cast<uint8_t>(std::clamp(
      next, static_cast<int>(valueEditMin), static_cast<int>(valueEditMax)));
}

void ReaderSettingsActivity::applyValueEdit() {
  if (!valueEditMode) {
    return;
  }
  const auto* settings = settingsForCategory(valueEditCategoryIndex);
  if (!settings || valueEditSettingIndex < 0 ||
      valueEditSettingIndex >= static_cast<int>(settings->size())) {
    valueEditMode = false;
    return;
  }

  const auto& setting = (*settings)[valueEditSettingIndex];
  SETTINGS.*(setting.valuePtr) = valueEditDraft;
  // In uniform mode, sync all margin fields when any margin is changed
  if (SETTINGS.uniformMargins &&
      setting.valuePtr == &CrossPointSettings::screenMarginHorizontal) {
    SETTINGS.screenMarginTop = valueEditDraft;
    SETTINGS.screenMarginBottom = valueEditDraft;
  }
  valueEditMode = false;
  persistSettings("reader settings value");
}

std::string ReaderSettingsActivity::currentValueEditText() const {
  if (!valueEditMode) {
    return {};
  }
  const auto* settings = settingsForCategory(valueEditCategoryIndex);
  if (!settings || valueEditSettingIndex < 0 ||
      valueEditSettingIndex >= static_cast<int>(settings->size())) {
    return {};
  }

  std::string valueText = std::to_string(valueEditDraft);
  const auto& setting = (*settings)[valueEditSettingIndex];
  if (setting.valuePtr == &CrossPointSettings::lineSpacingPercent) {
    valueText += "%";
  }
  return valueText;
}

void ReaderSettingsActivity::buildSettingsList() {
  // Free old memory fully before allocating new entries.
  { std::vector<SettingInfo>().swap(readerSettings); }
  { std::vector<SettingInfo>().swap(statusBarSettings); }
  { std::vector<FlatSettingRow>().swap(flatRows); }

  // Pre-allocate to avoid repeated reallocations (critical on low-heap devices)
  readerSettings.reserve(15);
  statusBarSettings.reserve(17);

  const bool txt = isTxtContext();

  // --- Helper: conditionally push a reader setting, applying filters ---
  auto pushReader = [&](SettingInfo&& s) {
    // Skip entries never shown in the in-reader settings screen
    if (s.valuePtr == &CrossPointSettings::orientation ||
        s.valuePtr == &CrossPointSettings::debugBorders ||
        s.valuePtr == &CrossPointSettings::textAntiAliasing) {
      return;
    }
    if (txt && s.valuePtr == &CrossPointSettings::readerStyleMode) {
      return;
    }
    // Filter margin entries based on dynamic/uniform/separate mode
    if (SETTINGS.dynamicMargins) {
      // When dynamic margins is on, hide all manual horizontal margin controls
      if (s.nameId == StrId::STR_UNIFORM_MARGINS ||
          s.nameId == StrId::STR_SCREEN_MARGIN ||
          s.nameId == StrId::STR_SCREEN_MARGIN_HORIZONTAL) {
        return;
      }
    } else if (SETTINGS.uniformMargins) {
      if (s.nameId == StrId::STR_SCREEN_MARGIN_HORIZONTAL ||
          s.nameId == StrId::STR_SCREEN_MARGIN_TOP ||
          s.nameId == StrId::STR_SCREEN_MARGIN_BOTTOM) {
        return;
      }
    } else {
      if (s.nameId == StrId::STR_SCREEN_MARGIN) {
        return;
      }
    }
    // TXT: limit paragraph alignment options
    if (txt && s.valuePtr == &CrossPointSettings::paragraphAlignment) {
      s.enumValues.resize(4);
      if (SETTINGS.paragraphAlignment == CrossPointSettings::BOOK_STYLE) {
        SETTINGS.paragraphAlignment = CrossPointSettings::JUSTIFIED;
      }
    }
    readerSettings.push_back(std::move(s));
  };

  // --- Build reader settings directly (no intermediate vector) ---
  pushReader(SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
      {StrId::STR_CHAREINK, StrId::STR_BOOKERLY, StrId::STR_VOLLKORN}));
  pushReader(SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
      {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE}));
  pushReader(SettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacingPercent, {65, 150, 5}));
  pushReader(SettingInfo::Toggle(StrId::STR_DYNAMIC_MARGINS, &CrossPointSettings::dynamicMargins));
  pushReader(SettingInfo::Toggle(StrId::STR_UNIFORM_MARGINS, &CrossPointSettings::uniformMargins));
  pushReader(SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMarginHorizontal, {0, 55, 5}));
  pushReader(SettingInfo::Value(StrId::STR_SCREEN_MARGIN_HORIZONTAL, &CrossPointSettings::screenMarginHorizontal, {0, 55, 5}));
  pushReader(SettingInfo::Value(StrId::STR_SCREEN_MARGIN_TOP, &CrossPointSettings::screenMarginTop, {0, 55, 5}));
  pushReader(SettingInfo::Value(StrId::STR_SCREEN_MARGIN_BOTTOM, &CrossPointSettings::screenMarginBottom, {0, 55, 5}));
  pushReader(SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
      {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE}));
  pushReader(SettingInfo::Enum(StrId::STR_FIRST_LINE_INDENT, &CrossPointSettings::firstLineIndentMode,
      {StrId::STR_BOOK_STYLE_OPT, StrId::STR_NONE_OPT, StrId::STR_INDENT_SMALL, StrId::STR_INDENT_MEDIUM, StrId::STR_INDENT_LARGE}));
  pushReader(SettingInfo::Enum(StrId::STR_READER_STYLE_MODE, &CrossPointSettings::readerStyleMode,
      {StrId::STR_READER_STYLE_USER, StrId::STR_READER_STYLE_HYBRID}));
  // Highlight mode removed — word-based selection is the only mode
  pushReader(SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW}));
  pushReader(SettingInfo::Enum(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacingPercent,
      {StrId::STR_WSPACING_M30, StrId::STR_WSPACING_0, StrId::STR_WSPACING_P80, StrId::STR_WSPACING_P150, StrId::STR_WSPACING_P240}));
  pushReader(SettingInfo::Enum(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacingLevel,
      {StrId::STR_NONE_OPT, StrId::STR_PARA_SPACING_17, StrId::STR_PARA_SPACING_25, StrId::STR_PARA_SPACING_33}));
  pushReader(SettingInfo::Enum(StrId::STR_TEXT_RENDER_MODE, &CrossPointSettings::textRenderMode,
      {StrId::STR_RENDER_CRISP, StrId::STR_RENDER_DARK, StrId::STR_RENDER_BIONIC}));
  if (!txt) {
    readerSettings.push_back(SettingInfo::Toggle(
        StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled));
  }

  // --- Build status bar settings directly ---
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_BAR, &CrossPointSettings::statusBarEnabled));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_FONT_SIZE, &CrossPointSettings::statusBarFontSize,
      {StrId::STR_STATUS_FONT_MIN, StrId::STR_STATUS_FONT_MAX}));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_BAR_THICKNESS, &CrossPointSettings::statusBarBarThickness,
      {StrId::STR_STATUS_BAR_THICKNESS_NORMAL, StrId::STR_STATUS_BAR_THICKNESS_DOUBLE}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_BATTERY, &CrossPointSettings::statusBarShowBattery));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_BATTERY_POSITION, &CrossPointSettings::statusBarBatteryPosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_PAGE_COUNTER, &CrossPointSettings::statusBarShowPageCounter));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_PAGE_COUNTER_MODE, &CrossPointSettings::statusBarPageCounterMode,
      {StrId::STR_STATUS_PAGE_MODE_CURRENT_TOTAL, StrId::STR_STATUS_PAGE_MODE_LEFT_CHAPTER}));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_PAGE_COUNTER_POSITION, &CrossPointSettings::statusBarPageCounterPosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_BOOK_PERCENT, &CrossPointSettings::statusBarShowBookPercentage));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_BOOK_PERCENT_POSITION, &CrossPointSettings::statusBarBookPercentagePosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_PERCENT, &CrossPointSettings::statusBarShowChapterPercentage));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_CHAPTER_PERCENT_POSITION, &CrossPointSettings::statusBarChapterPercentagePosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_BOOK_BAR, &CrossPointSettings::statusBarShowBookBar));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_BOOK_BAR_POSITION, &CrossPointSettings::statusBarBookBarPosition,
      {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_BAR, &CrossPointSettings::statusBarShowChapterBar));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_CHAPTER_BAR_POSITION, &CrossPointSettings::statusBarChapterBarPosition,
      {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_TITLE, &CrossPointSettings::statusBarShowChapterTitle));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_CHAPTER_TITLE_POSITION, &CrossPointSettings::statusBarTitlePosition,
      {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM}));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_NO_TITLE_TRUNCATION, &CrossPointSettings::statusBarNoTitleTruncation));
  statusBarSettings.push_back(SettingInfo::Toggle(StrId::STR_STATUS_BOOK_PAGE_COUNTER, &CrossPointSettings::statusBarShowBookPageCounter));
  statusBarSettings.push_back(SettingInfo::Enum(StrId::STR_STATUS_BOOK_PAGE_COUNTER_POSITION, &CrossPointSettings::statusBarBookPageCounterPosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));

  // --- Build flat row index ---
  flatRows.reserve(2 + readerSettings.size() + statusBarSettings.size());
  for (int categoryIndex = 0; categoryIndex < 2; categoryIndex++) {
    flatRows.push_back(
        FlatSettingRow{.isHeader = true, .categoryIndex = categoryIndex});
    const auto* settings = settingsForCategory(categoryIndex);
    for (size_t i = 0; i < settings->size(); i++) {
      flatRows.push_back(FlatSettingRow{.isHeader = false,
                                        .categoryIndex = categoryIndex,
                                        .settingIndex = static_cast<int>(i)});
    }
  }
}

void ReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  buildSettingsList();
  selectedRowIndex = findNextEditableRow(0, +1);
  requestUpdate();
}

void ReaderSettingsActivity::onExit() {
  Activity::onExit();
  fontSizeEditMode = false;
  valueEditMode = false;
}

void ReaderSettingsActivity::toggleCurrentSetting() {
  if (selectedRowIndex < 0 || selectedRowIndex >= static_cast<int>(flatRows.size())) {
    return;
  }
  const auto& row = flatRows[selectedRowIndex];
  if (row.isHeader) {
    return;
  }

  const auto* settings = settingsForCategory(row.categoryIndex);
  if (!settings || row.settingIndex < 0 ||
      row.settingIndex >= static_cast<int>(settings->size())) {
    return;
  }

  const auto& setting = (*settings)[row.settingIndex];
  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    SETTINGS.*(setting.valuePtr) = !(SETTINGS.*(setting.valuePtr));
    if (setting.valuePtr == &CrossPointSettings::uniformMargins) {
      if (SETTINGS.uniformMargins) {
        // Switching to uniform: sync all margins to horizontal value
        SETTINGS.screenMarginTop = SETTINGS.screenMarginHorizontal;
        SETTINGS.screenMarginBottom = SETTINGS.screenMarginHorizontal;
      }
      buildSettingsList();
      selectedRowIndex =
          std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    }
    if (setting.valuePtr == &CrossPointSettings::dynamicMargins) {
      buildSettingsList();
      selectedRowIndex =
          std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    if (setting.valuePtr == &CrossPointSettings::fontSize) {
      startFontSizeEdit();
      return;
    } else if (setting.valuePtr == &CrossPointSettings::fontFamily) {
      const uint8_t currentIndex =
          CrossPointSettings::fontFamilyToDisplayIndex(SETTINGS.fontFamily);
      SETTINGS.fontFamily = CrossPointSettings::displayIndexToFontFamily(
          (currentIndex + 1) % static_cast<uint8_t>(setting.enumValues.size()));
      SETTINGS.fontFamily =
          CrossPointSettings::normalizeFontFamily(SETTINGS.fontFamily);
      SETTINGS.fontSize = CrossPointSettings::normalizeFontSizeForFamily(
          SETTINGS.fontFamily, SETTINGS.fontSize);
      SETTINGS.lineSpacingPercent = 90;  // Reset to default on font change
      buildSettingsList();
      selectedRowIndex =
          std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    } else {
      const int currentValue = SETTINGS.*(setting.valuePtr);
      SETTINGS.*(setting.valuePtr) =
          (currentValue + 1) % static_cast<uint8_t>(setting.enumValues.size());
    }
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    if (isPopupValueSetting(setting)) {
      startValueEdit(setting, row.categoryIndex, row.settingIndex);
      return;
    }
    const int currentValue = SETTINGS.*(setting.valuePtr);
    if (currentValue + setting.valueRange.step > setting.valueRange.max) {
      SETTINGS.*(setting.valuePtr) = setting.valueRange.min;
    } else {
      SETTINGS.*(setting.valuePtr) = currentValue + setting.valueRange.step;
    }
  } else {
    return;
  }

  persistSettings("reader settings toggle");
}

void ReaderSettingsActivity::loop() {
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
      valueEditMode = false;
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
      if (!settings || valueEditSettingIndex < 0 ||
          valueEditSettingIndex >= static_cast<int>(settings->size())) {
        return;
      }
      adjustValueEdit(+getValueEditHoldStep(mappedInput,
                                            (*settings)[valueEditSettingIndex]));
      requestUpdate();
    });
    buttonNavigator.onPreviousContinuous([this] {
      const auto* settings = settingsForCategory(valueEditCategoryIndex);
      if (!settings || valueEditSettingIndex < 0 ||
          valueEditSettingIndex >= static_cast<int>(settings->size())) {
        return;
      }
      adjustValueEdit(-getValueEditHoldStep(mappedInput,
                                            (*settings)[valueEditSettingIndex]));
      requestUpdate();
    });
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onClose(dirty);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    requestUpdate();
    return;
  }

  buttonNavigator.onNextRelease([this] {
    selectedRowIndex = findNextEditableRow(selectedRowIndex, +1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedRowIndex = findNextEditableRow(selectedRowIndex, -1);
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

void ReaderSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto metrics = UITheme::getInstance().getMetrics();

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + 5,
                            tr(STR_READER_SETTINGS), true,
                            EpdFontFamily::REGULAR);

  const int contentY = metrics.topPadding + metrics.headerHeight;
  const int contentHeight =
      pageHeight - (contentY + metrics.buttonHintsHeight + metrics.verticalSpacing);
  const int rowHeight = metrics.listRowHeight;
  const int pageItems = std::max(1, contentHeight / rowHeight);
  const int pageStartIndex = (selectedRowIndex / pageItems) * pageItems;

  for (int i = pageStartIndex;
       i < static_cast<int>(flatRows.size()) && i < pageStartIndex + pageItems;
       i++) {
    const int rowY = contentY + (i - pageStartIndex) * rowHeight;
    const auto& row = flatRows[i];

    if (row.isHeader) {
      renderer.fillRect(0, rowY, pageWidth, rowHeight, true);
      const char* label = I18N.get(kCategoryNames[row.categoryIndex]);
      const int textW =
          renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, (pageWidth - textW) / 2, rowY, label,
                        false, EpdFontFamily::REGULAR);
      continue;
    }

    const auto* settings = settingsForCategory(row.categoryIndex);
    const auto& setting = (*settings)[row.settingIndex];
    const bool isSelected = (i == selectedRowIndex);
    const char* settingName = I18N.get(setting.nameId);
    constexpr int kChipPad = 1;
    const int textH = renderer.getTextHeight(UI_10_FONT_ID);
    const int chipH = textH + kChipPad * 2;
    const int chipY = rowY + (rowHeight - chipH) / 2;

    if (isSelected) {
      const int nameWidth = renderer.getTextWidth(UI_10_FONT_ID, settingName);
      renderer.fillRect(metrics.contentSidePadding - kChipPad, chipY,
                        nameWidth + kChipPad * 2, chipH, true);
    }
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, rowY,
                      settingName, !isSelected);

    std::string valueText;
    if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
      valueText =
          (SETTINGS.*(setting.valuePtr)) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
      if (setting.valuePtr == &CrossPointSettings::fontSize) {
        valueText = fontSizeValueLabel(SETTINGS.fontFamily, SETTINGS.fontSize);
      } else if (setting.valuePtr == &CrossPointSettings::fontFamily) {
        valueText = I18N.get(
            setting.enumValues[CrossPointSettings::fontFamilyToDisplayIndex(
                SETTINGS.fontFamily)]);
      } else {
        valueText = I18N.get(setting.enumValues[SETTINGS.*(setting.valuePtr)]);
      }
    } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
      const uint8_t valueToShow =
          (valueEditMode && row.categoryIndex == valueEditCategoryIndex &&
           row.settingIndex == valueEditSettingIndex)
              ? valueEditDraft
              : SETTINGS.*(setting.valuePtr);
      valueText = std::to_string(valueToShow);
      if (setting.valuePtr == &CrossPointSettings::lineSpacingPercent) {
        valueText += "%";
      }
    }

    if (!valueText.empty()) {
      const int valueW = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      const int valueX = pageWidth - metrics.contentSidePadding - valueW;
      if (isSelected) {
        renderer.fillRect(valueX - kChipPad, chipY, valueW + kChipPad * 2, chipH, true);
      }
      renderer.drawText(UI_10_FONT_ID, valueX, rowY,
                        valueText.c_str(), !isSelected);
    }
  }

  const char* confirmLabel =
      (valueEditMode || fontSizeEditMode) ? tr(STR_CONFIRM) : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel,
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3,
                      labels.btn4);

  if (fontSizeEditMode) {
    // Show all available font sizes in a row: e.g. "14  15  [16]  17  18"
    // with the selected one highlighted. No font preview rendering to avoid
    // decompressing a new font group (OOM on large books).
    const int optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
    const int textH = renderer.getTextHeight(UI_12_FONT_ID);
    constexpr int kItemPadH = 4;   // horizontal padding inside each chip
    constexpr int kItemPadV = 3;   // vertical padding inside selected chip
    constexpr int kItemGap = 10;   // gap between items
    constexpr int kPopupPad = 16;  // padding inside popup

    // Measure total width of all size labels
    int totalItemsW = 0;
    for (int i = 0; i < optionCount; i++) {
      const uint8_t fs = CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, i);
      const std::string label = fontSizeValueLabel(SETTINGS.fontFamily, fs);
      totalItemsW += renderer.getTextWidth(UI_12_FONT_ID, label.c_str()) + kItemPadH * 2;
    }
    totalItemsW += kItemGap * std::max(0, optionCount - 1);

    const char* title = tr(STR_FONT_SIZE);
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, title);
    const int contentW = std::max(totalItemsW, titleW);
    const int popupW = std::min(pageWidth - 20, contentW + kPopupPad * 2);
    const int popupH = 54;
    const int popupX = (pageWidth - popupW) / 2;
    const int popupY = (pageHeight - popupH) / 2;

    renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
    renderer.fillRect(popupX, popupY, popupW, popupH, false);
    renderer.drawText(UI_10_FONT_ID, popupX + (popupW - titleW) / 2,
                      popupY + 6, title, true);

    // Draw size options in a row, selected one highlighted
    int curX = popupX + (popupW - totalItemsW) / 2;
    const int itemY = popupY + 28;
    for (int i = 0; i < optionCount; i++) {
      const uint8_t fs = CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, i);
      const std::string label = fontSizeValueLabel(SETTINGS.fontFamily, fs);
      const int labelW = renderer.getTextWidth(UI_12_FONT_ID, label.c_str());
      const int chipW = labelW + kItemPadH * 2;
      const bool isSelected = (i == static_cast<int>(fontSizeEditDraftIndex));

      if (isSelected) {
        renderer.fillRect(curX, itemY - kItemPadV, chipW,
                          textH + kItemPadV * 2, true);
      }
      renderer.drawText(UI_12_FONT_ID, curX + kItemPadH, itemY,
                        label.c_str(), !isSelected);
      curX += chipW + kItemGap;
    }
  }

  if (valueEditMode) {
    const auto* settings = settingsForCategory(valueEditCategoryIndex);
    if (settings && valueEditSettingIndex >= 0 &&
        valueEditSettingIndex < static_cast<int>(settings->size())) {
      const auto& setting = (*settings)[valueEditSettingIndex];
      const char* settingLabel = I18N.get(setting.nameId);
      const std::string valueText = currentValueEditText();

      // Auto-size popup to fit content
      constexpr int kPopupPad = 20;
      const int titleW = renderer.getTextWidth(UI_10_FONT_ID, settingLabel);
      const int valueW = renderer.getTextWidth(UI_12_FONT_ID, valueText.c_str());
      const int minBarW = 120;
      const int contentW = std::max({titleW, valueW, minBarW});
      const int popupW = std::min(pageWidth - 20, contentW + kPopupPad * 2);
      const int popupH = 86;
      const int popupX = (pageWidth - popupW) / 2;
      const int popupY = (pageHeight - popupH) / 2;

      renderer.fillRect(popupX - 2, popupY - 2, popupW + 4, popupH + 4, true);
      renderer.fillRect(popupX, popupY, popupW, popupH, false);

      renderer.drawText(UI_10_FONT_ID, popupX + (popupW - titleW) / 2,
                        popupY + 8, settingLabel, true);

      renderer.drawText(UI_12_FONT_ID, popupX + (popupW - valueW) / 2,
                        popupY + 30, valueText.c_str(), true);

      const int barX = popupX + kPopupPad;
      const int barY = popupY + popupH - 22;
      const int barW = popupW - kPopupPad * 2;
      const int barH = 8;
      renderer.drawRect(barX, barY, barW, barH, true);
      const int range =
          std::max(1, static_cast<int>(valueEditMax) - static_cast<int>(valueEditMin));
      const int filledW =
          2 + ((static_cast<int>(valueEditDraft) -
                static_cast<int>(valueEditMin)) *
               std::max(1, barW - 4)) /
                  range;
      renderer.fillRect(barX + 2, barY + 2, filledW, std::max(1, barH - 4),
                        true);
    }
  }

  renderer.displayBuffer();
}
