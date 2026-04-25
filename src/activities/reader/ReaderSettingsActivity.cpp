#include "ReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingThemeStore.h"
#include "components/themes/BaseTheme.h"
#include "fonts/CustomBinFontManager.h"
#include "util/TransitionFeedback.h"

namespace {
const StrId kCategoryNames[] = {StrId::STR_CAT_READER, StrId::STR_STATUS_BAR};

std::string fontSizeValueLabel(const uint8_t family, const uint8_t fontSize) {
  if (family == CrossPointSettings::CUSTOM_FAMILY) {
    // For custom families the "fontSize" byte is not the source of truth
    // — customFontSizePt is. Return the persisted pixel size so the
    // picker rows show "18", "22" etc. During edit mode the caller
    // passes the draft size via fontSize to render a live preview.
    return std::to_string(fontSize);
  }
  return std::to_string(CrossPointSettings::fontSizeToPointSize(family, fontSize));
}

int getValueEditHoldStep(const MappedInputManager& mappedInput, const ReaderSettingInfo&) {
  return mappedInput.getHeldTime() >= 3000 ? 10 : 1;
}

}  // namespace

bool ReaderSettingsActivity::isTxtContext() const { return bookCachePath.find("/txt_") != std::string::npos; }

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

const std::vector<ReaderSettingInfo>* ReaderSettingsActivity::settingsForCategory(const int categoryIndex) const {
  switch (categoryIndex) {
    case 0:
      return &readerSettings;
    case 1:
    default:
      return &statusBarSettings;
  }
}

int ReaderSettingsActivity::findNextEditableRow(const int startIndex, const int direction) const {
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

bool ReaderSettingsActivity::isPopupValueSetting(const ReaderSettingInfo& setting) const {
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
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
    // Draft index into the dynamic sizes-for-family list rather than the
    // built-in enum table. Snaps to 0 if the currently-persisted size
    // isn't in the list (e.g. the family was re-scanned and that size
    // was deleted between reader opens).
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

void ReaderSettingsActivity::adjustFontSizeEdit(const int delta) {
  int optionCount = 0;
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
    optionCount = static_cast<int>(
        crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName).size());
    if (optionCount == 0) optionCount = 1;  // avoid clamp to [0, -1]
  } else {
    optionCount = CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
  }
  const int next = static_cast<int>(fontSizeEditDraftIndex) + delta;
  fontSizeEditDraftIndex = static_cast<uint8_t>(std::clamp(next, 0, std::max(0, optionCount - 1)));
}

void ReaderSettingsActivity::applyFontSizeEdit() {
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
        TransitionFeedback::show(renderer, tr(STR_FONT_LOAD_FAILED));
        fontSizeEditMode = false;
        return;
      }
      SETTINGS.customFontSizePt = trialSize;
    }
  } else {
    SETTINGS.fontSize = CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, fontSizeEditDraftIndex);
  }
  fontSizeEditMode = false;
  persistSettings("reader settings font size");
}

void ReaderSettingsActivity::startValueEdit(const ReaderSettingInfo& setting, const int categoryIndex,
                                            const int settingIndex) {
  valueEditMode = true;
  valueEditCategoryIndex = categoryIndex;
  valueEditSettingIndex = settingIndex;
  valueEditMin = setting.valueRange.min;
  valueEditMax = setting.valueRange.max;
  valueEditDraft = std::clamp(SETTINGS.*(setting.valuePtr), valueEditMin, valueEditMax);
}

void ReaderSettingsActivity::adjustValueEdit(const int delta) {
  const int next = static_cast<int>(valueEditDraft) + delta;
  valueEditDraft =
      static_cast<uint8_t>(std::clamp(next, static_cast<int>(valueEditMin), static_cast<int>(valueEditMax)));
}

void ReaderSettingsActivity::applyValueEdit() {
  if (!valueEditMode) {
    return;
  }
  const auto* settings = settingsForCategory(valueEditCategoryIndex);
  if (!settings || valueEditSettingIndex < 0 || valueEditSettingIndex >= static_cast<int>(settings->size())) {
    valueEditMode = false;
    return;
  }

  const auto& setting = (*settings)[valueEditSettingIndex];
  SETTINGS.*(setting.valuePtr) = valueEditDraft;
  // In uniform mode, sync all margin fields when any margin is changed
  if (SETTINGS.uniformMargins && setting.valuePtr == &CrossPointSettings::screenMarginHorizontal) {
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
  if (!settings || valueEditSettingIndex < 0 || valueEditSettingIndex >= static_cast<int>(settings->size())) {
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
  {
    std::vector<ReaderSettingInfo>().swap(readerSettings);
  }
  {
    std::vector<ReaderSettingInfo>().swap(statusBarSettings);
  }
  {
    std::vector<FlatSettingRow>().swap(flatRows);
  }

  // Pre-allocate to avoid repeated reallocations (critical on low-heap devices)
  readerSettings.reserve(15);
  statusBarSettings.reserve(17);

  const bool txt = isTxtContext();

  // --- Helper: conditionally push a reader setting, applying filters ---
  auto pushReader = [&](ReaderSettingInfo&& s) {
    // Skip entries never shown in the in-reader settings screen
    if (s.valuePtr == &CrossPointSettings::orientation || s.valuePtr == &CrossPointSettings::debugBorders ||
        s.valuePtr == &CrossPointSettings::textAntiAliasing) {
      return;
    }
    if (txt && s.valuePtr == &CrossPointSettings::readerStyleMode) {
      return;
    }
    // Filter margin entries based on dynamic/uniform/separate mode
    if (SETTINGS.dynamicMargins) {
      // When dynamic margins is on, hide all manual horizontal margin controls
      if (s.nameId == StrId::STR_UNIFORM_MARGINS || s.nameId == StrId::STR_SCREEN_MARGIN ||
          s.nameId == StrId::STR_SCREEN_MARGIN_HORIZONTAL) {
        return;
      }
    } else if (SETTINGS.uniformMargins) {
      if (s.nameId == StrId::STR_SCREEN_MARGIN_HORIZONTAL || s.nameId == StrId::STR_SCREEN_MARGIN_TOP ||
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
  {
    // 5 built-in entries (order matches fontFamilyToDisplayIndex: CHAREINK,
    // BOOKERLY, VOLLKORN, GALMURI, BITTER). Beyond that the dynamic tail
    // appends N "Custom: <name>" rows — one per installed custom family
    // — using the raw-string dynamicLabels path. Display index 5+ selects
    // the n-th installed custom family and writes its name into
    // customFontName.
    ReaderSettingInfo familyPicker = ReaderSettingInfo::Enum(
        StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
        {StrId::STR_CHAREINK, StrId::STR_BOOKERLY, StrId::STR_VOLLKORN, StrId::STR_GALMURI, StrId::STR_BITTER});
    const auto names = crosspoint::fonts::CustomBinFontManager::instance().familyNames();
    for (const auto& name : names) {
      std::string label = std::string(I18N.get(StrId::STR_CUSTOM_PREFIX)) + name;
      familyPicker.dynamicLabels.push_back(std::move(label));
    }
    pushReader(std::move(familyPicker));
  }
  pushReader(ReaderSettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                                     {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE}));
  pushReader(ReaderSettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacingPercent, {35, 150, 5}));
  pushReader(ReaderSettingInfo::Enum(
      StrId::STR_DYNAMIC_MARGINS, &CrossPointSettings::dynamicMargins,
      {StrId::STR_DYNAMIC_MARGINS_OFF, StrId::STR_DYNAMIC_MARGINS_10, StrId::STR_DYNAMIC_MARGINS_20}));
  pushReader(ReaderSettingInfo::Toggle(StrId::STR_UNIFORM_MARGINS, &CrossPointSettings::uniformMargins));
  pushReader(
      ReaderSettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMarginHorizontal, {0, 55, 5}));
  pushReader(ReaderSettingInfo::Value(StrId::STR_SCREEN_MARGIN_HORIZONTAL, &CrossPointSettings::screenMarginHorizontal,
                                      {0, 55, 5}));
  pushReader(ReaderSettingInfo::Value(StrId::STR_SCREEN_MARGIN_TOP, &CrossPointSettings::screenMarginTop, {0, 55, 5}));
  pushReader(
      ReaderSettingInfo::Value(StrId::STR_SCREEN_MARGIN_BOTTOM, &CrossPointSettings::screenMarginBottom, {0, 55, 5}));
  pushReader(ReaderSettingInfo::Enum(
      StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
      {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE}));
  pushReader(ReaderSettingInfo::Enum(StrId::STR_FIRST_LINE_INDENT, &CrossPointSettings::firstLineIndentMode,
                                     {StrId::STR_BOOK_STYLE_OPT, StrId::STR_NONE_OPT, StrId::STR_INDENT_SMALL,
                                      StrId::STR_INDENT_MEDIUM, StrId::STR_INDENT_LARGE}));
  pushReader(ReaderSettingInfo::Enum(StrId::STR_READER_STYLE_MODE, &CrossPointSettings::readerStyleMode,
                                     {StrId::STR_READER_STYLE_USER, StrId::STR_READER_STYLE_HYBRID}));
  // Highlight mode removed — word-based selection is the only mode
  pushReader(ReaderSettingInfo::Enum(
      StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW}));
  pushReader(ReaderSettingInfo::Enum(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacingPercent,
                                     {StrId::STR_WSPACING_M30, StrId::STR_WSPACING_0, StrId::STR_WSPACING_P80,
                                      StrId::STR_WSPACING_P150, StrId::STR_WSPACING_P240}));
  pushReader(ReaderSettingInfo::Enum(
      StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacingLevel,
      {StrId::STR_NONE_OPT, StrId::STR_PARA_SPACING_17, StrId::STR_PARA_SPACING_25, StrId::STR_PARA_SPACING_33}));
  pushReader(ReaderSettingInfo::Enum(StrId::STR_TEXT_RENDER_MODE, &CrossPointSettings::textRenderMode,
                                     {StrId::STR_RENDER_CRISP, StrId::STR_RENDER_DARK, StrId::STR_RENDER_BIONIC}));
  if (!txt) {
    readerSettings.push_back(
        ReaderSettingInfo::Toggle(StrId::STR_HYPHENATION, &CrossPointSettings::hyphenationEnabled));
  }

  // --- Build status bar settings directly ---
  statusBarSettings.push_back(ReaderSettingInfo::Toggle(StrId::STR_STATUS_BAR, &CrossPointSettings::statusBarEnabled));
  statusBarSettings.push_back(ReaderSettingInfo::Enum(StrId::STR_STATUS_FONT_SIZE,
                                                      &CrossPointSettings::statusBarFontSize,
                                                      {StrId::STR_STATUS_FONT_MIN, StrId::STR_STATUS_FONT_MAX}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Enum(StrId::STR_STATUS_BAR_THICKNESS, &CrossPointSettings::statusBarBarThickness,
                              {StrId::STR_STATUS_BAR_THICKNESS_NORMAL, StrId::STR_STATUS_BAR_THICKNESS_DOUBLE}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Toggle(StrId::STR_STATUS_BATTERY, &CrossPointSettings::statusBarShowBattery));
  statusBarSettings.push_back(ReaderSettingInfo::Enum(
      StrId::STR_STATUS_BATTERY_POSITION, &CrossPointSettings::statusBarBatteryPosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Toggle(StrId::STR_STATUS_PAGE_COUNTER, &CrossPointSettings::statusBarShowPageCounter));
  statusBarSettings.push_back(
      ReaderSettingInfo::Enum(StrId::STR_STATUS_PAGE_COUNTER_MODE, &CrossPointSettings::statusBarPageCounterMode,
                              {StrId::STR_STATUS_PAGE_MODE_CURRENT_TOTAL, StrId::STR_STATUS_PAGE_MODE_LEFT_CHAPTER}));
  statusBarSettings.push_back(ReaderSettingInfo::Enum(
      StrId::STR_STATUS_PAGE_COUNTER_POSITION, &CrossPointSettings::statusBarPageCounterPosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Toggle(StrId::STR_STATUS_BOOK_PERCENT, &CrossPointSettings::statusBarShowBookPercentage));
  statusBarSettings.push_back(ReaderSettingInfo::Enum(
      StrId::STR_STATUS_BOOK_PERCENT_POSITION, &CrossPointSettings::statusBarBookPercentagePosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(ReaderSettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_PERCENT,
                                                        &CrossPointSettings::statusBarShowChapterPercentage));
  statusBarSettings.push_back(ReaderSettingInfo::Enum(
      StrId::STR_STATUS_CHAPTER_PERCENT_POSITION, &CrossPointSettings::statusBarChapterPercentagePosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Toggle(StrId::STR_STATUS_BOOK_BAR, &CrossPointSettings::statusBarShowBookBar));
  statusBarSettings.push_back(
      ReaderSettingInfo::Enum(StrId::STR_STATUS_BOOK_BAR_POSITION, &CrossPointSettings::statusBarBookBarPosition,
                              {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_BAR, &CrossPointSettings::statusBarShowChapterBar));
  statusBarSettings.push_back(
      ReaderSettingInfo::Enum(StrId::STR_STATUS_CHAPTER_BAR_POSITION, &CrossPointSettings::statusBarChapterBarPosition,
                              {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM}));
  statusBarSettings.push_back(
      ReaderSettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_TITLE, &CrossPointSettings::statusBarShowChapterTitle));
  statusBarSettings.push_back(
      ReaderSettingInfo::Enum(StrId::STR_STATUS_CHAPTER_TITLE_POSITION, &CrossPointSettings::statusBarTitlePosition,
                              {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM}));
  statusBarSettings.push_back(ReaderSettingInfo::Toggle(StrId::STR_STATUS_NO_TITLE_TRUNCATION,
                                                        &CrossPointSettings::statusBarNoTitleTruncation));
  statusBarSettings.push_back(ReaderSettingInfo::Toggle(StrId::STR_STATUS_BOOK_PAGE_COUNTER,
                                                        &CrossPointSettings::statusBarShowBookPageCounter));
  statusBarSettings.push_back(ReaderSettingInfo::Enum(
      StrId::STR_STATUS_BOOK_PAGE_COUNTER_POSITION, &CrossPointSettings::statusBarBookPageCounterPosition,
      {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER, StrId::STR_STATUS_POS_TOP_RIGHT,
       StrId::STR_STATUS_POS_BOTTOM_LEFT, StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT}));

  // --- Build flat row index ---
  flatRows.reserve(2 + readerSettings.size() + statusBarSettings.size());
  for (int categoryIndex = 0; categoryIndex < 2; categoryIndex++) {
    flatRows.push_back(FlatSettingRow{.isHeader = true, .categoryIndex = categoryIndex});
    const auto* settings = settingsForCategory(categoryIndex);
    for (size_t i = 0; i < settings->size(); i++) {
      flatRows.push_back(
          FlatSettingRow{.isHeader = false, .categoryIndex = categoryIndex, .settingIndex = static_cast<int>(i)});
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
  if (!settings || row.settingIndex < 0 || row.settingIndex >= static_cast<int>(settings->size())) {
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
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    if (setting.valuePtr == &CrossPointSettings::fontSize) {
      startFontSizeEdit();
      return;
    } else if (setting.valuePtr == &CrossPointSettings::fontFamily) {
      // Font-family picker = 6 built-in StrIds + N deduped custom names.
      // Cycling resolves against the sum. Landing on a custom slot sets
      // fontFamily=CUSTOM_FAMILY + customFontName + customFontSizePt
      // (smallest available for that name, preserving prior choice if
      // still valid for the new family).
      const auto names = crosspoint::fonts::CustomBinFontManager::instance().familyNames();
      const size_t builtinCount = setting.enumValues.size();  // 6
      const size_t customCount = names.size();
      const size_t total = builtinCount + customCount;
      size_t currentIndex = CrossPointSettings::fontFamilyToDisplayIndex(SETTINGS.fontFamily);
      if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
        currentIndex = builtinCount;  // default to first custom if name not found
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
      const auto prevFontSize = SETTINGS.fontSize;
      const auto prevLineSpacing = SETTINGS.lineSpacingPercent;

      if (nextIndex < builtinCount) {
        SETTINGS.fontFamily = CrossPointSettings::displayIndexToFontFamily(static_cast<uint8_t>(nextIndex));
        SETTINGS.customFontName.clear();
        SETTINGS.customFontSizePt = 0;
      } else {
        SETTINGS.fontFamily = CrossPointSettings::CUSTOM_FAMILY;
        const size_t customSlot = nextIndex - builtinCount;
        SETTINGS.customFontName = names[customSlot];
        const auto sizes =
            crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName);
        // Keep the previously-selected size if it still exists for the
        // new family; otherwise pick the smallest so the picker has a
        // valid default.
        bool keep = false;
        for (auto s : sizes) {
          if (s == SETTINGS.customFontSizePt) {
            keep = true;
            break;
          }
        }
        if (!keep) SETTINGS.customFontSizePt = sizes.empty() ? 0 : sizes.front();
      }
      SETTINGS.fontFamily = CrossPointSettings::normalizeFontFamily(SETTINGS.fontFamily);
      SETTINGS.fontSize = CrossPointSettings::normalizeFontSizeForFamily(SETTINGS.fontFamily, SETTINGS.fontSize);
      SETTINGS.lineSpacingPercent = CrossPointSettings::resetLineSpacingPercentForFamily(SETTINGS.fontFamily);
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
        SETTINGS.fontSize = prevFontSize;
        SETTINGS.lineSpacingPercent = prevLineSpacing;
        TransitionFeedback::show(renderer, tr(STR_FONT_LOAD_FAILED));
        return;
      }
      buildSettingsList();
      selectedRowIndex = std::min(selectedRowIndex, static_cast<int>(flatRows.size()) - 1);
    } else {
      const int currentValue = SETTINGS.*(setting.valuePtr);
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
  const auto metrics = BaseMetrics::values;

  renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + 5, tr(STR_READER_SETTINGS), true,
                            EpdFontFamily::REGULAR);

  const int contentY = metrics.topPadding + metrics.headerHeight;
  const int contentHeight = pageHeight - (contentY + metrics.buttonHintsHeight + metrics.verticalSpacing);
  const int rowHeight = metrics.listRowHeight;
  const int rowFont = UI_10_FONT_ID;
  const int availableWidth = pageWidth - metrics.contentSidePadding * 2;
  const int wrapGap = metrics.contentSidePadding;
  constexpr int kChipPad = 1;

  // Helper lambdas mirror SettingsActivity — split a label into up to two
  // lines with greedy word-wrap and a codepoint-wrap fallback for very long
  // single words (some custom font family names are one big word).
  auto takePrefixFit = [&](const std::string& word, int maxW) -> std::pair<std::string, std::string> {
    int bestEnd = 0;
    for (int end = 1; end <= static_cast<int>(word.size()); end++) {
      if (end < static_cast<int>(word.size()) && (static_cast<uint8_t>(word[end]) & 0xC0) == 0x80) continue;
      const std::string candidate = word.substr(0, end);
      if (renderer.getTextWidth(rowFont, candidate.c_str()) <= maxW) {
        bestEnd = end;
      } else {
        break;
      }
    }
    if (bestEnd == 0) return {std::string(), word};
    return {word.substr(0, bestEnd), word.substr(bestEnd)};
  };

  auto splitLabel = [&](const char* label) -> std::pair<std::string, std::string> {
    std::string s(label);
    if (renderer.getTextWidth(rowFont, s.c_str()) <= availableWidth) return {s, std::string()};
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
      if (!cur.empty()) words.push_back(std::move(cur));
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
      if (!split.second.empty())
        words[wi] = split.second;
      else
        wi++;
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

  // computeValueText mirrors the original inline block so the pre-scan and
  // render loop share one source of truth for what the row shows.
  auto computeValueText = [&](const FlatSettingRow& row) -> std::string {
    if (row.isHeader) return std::string();
    const auto* settings = settingsForCategory(row.categoryIndex);
    const auto& setting = (*settings)[row.settingIndex];
    std::string v;
    if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
      v = (SETTINGS.*(setting.valuePtr)) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
      if (setting.valuePtr == &CrossPointSettings::fontSize) {
        const uint8_t size =
            (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) ? SETTINGS.customFontSizePt : SETTINGS.fontSize;
        v = fontSizeValueLabel(SETTINGS.fontFamily, size);
      } else if (setting.valuePtr == &CrossPointSettings::fontFamily) {
        if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
          const auto names = crosspoint::fonts::CustomBinFontManager::instance().familyNames();
          bool matched = false;
          for (size_t k = 0; k < names.size(); ++k) {
            if (names[k] == SETTINGS.customFontName) {
              if (k < setting.dynamicLabels.size()) v = setting.dynamicLabels[k];
              matched = true;
              break;
            }
          }
          if (!matched && !setting.dynamicLabels.empty())
            v = setting.dynamicLabels[0];
          else if (!matched)
            v = I18N.get(setting.enumValues[0]);
        } else {
          v = I18N.get(setting.enumValues[CrossPointSettings::fontFamilyToDisplayIndex(SETTINGS.fontFamily)]);
        }
      } else {
        v = I18N.get(setting.enumValues[SETTINGS.*(setting.valuePtr)]);
      }
    } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
      const uint8_t valueToShow =
          (valueEditMode && row.categoryIndex == valueEditCategoryIndex && row.settingIndex == valueEditSettingIndex)
              ? valueEditDraft
              : SETTINGS.*(setting.valuePtr);
      v = std::to_string(valueToShow);
      if (setting.valuePtr == &CrossPointSettings::lineSpacingPercent) v += "%";
    }
    return v;
  };

  // Pre-scan pass: for every row decide how its label wraps and whether the
  // value inlines next to the label or drops to a line below. Rows that do
  // not fit on one line grow to 2 (wrapped label OR dropped value) or 3
  // (wrapped label + dropped value) row-heights. Pagination then packs by
  // summed heights so we never clip a row mid-wrap.
  const int rowCount = static_cast<int>(flatRows.size());
  std::vector<std::string> valueTexts(rowCount);
  std::vector<std::string> labelLine1s(rowCount);
  std::vector<std::string> labelLine2s(rowCount);
  std::vector<uint8_t> valueLineOffset(rowCount, 0);
  std::vector<int> rowHeights(rowCount, rowHeight);

  for (int i = 0; i < rowCount; i++) {
    const auto& row = flatRows[i];
    if (row.isHeader) continue;
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

  // Page-pack by summed row heights so no row ever gets clipped mid-wrap.
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
    while (pageStartIndex < rowCount && pageOfRow[pageStartIndex] != targetPage) pageStartIndex++;
  }

  const int renderPage = (rowCount > 0) ? pageOfRow[pageStartIndex] : 0;
  int rowYOffset = 0;
  for (int i = pageStartIndex; i < rowCount && pageOfRow[i] == renderPage; i++) {
    const int rowY = contentY + rowYOffset;
    const auto& row = flatRows[i];
    const int thisRowHeight = rowHeights[i];

    if (row.isHeader) {
      renderer.fillRect(0, rowY, pageWidth, rowHeight, true);
      const char* label = I18N.get(kCategoryNames[row.categoryIndex]);
      const int textW = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, (pageWidth - textW) / 2, rowY, label, false, EpdFontFamily::REGULAR);
      rowYOffset += thisRowHeight;
      continue;
    }

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

  const char* confirmLabel = (valueEditMode || fontSizeEditMode) ? tr(STR_CONFIRM) : tr(STR_TOGGLE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (fontSizeEditMode) {
    // Show all available font sizes in a row: e.g. "14  15  [16]  17  18"
    // with the selected one highlighted. No font preview rendering to avoid
    // decompressing a new font group (OOM on large books). Custom families
    // bypass the built-in fontSize enum entirely and list every pixel size
    // the user has dropped BDFs for.
    const bool customFamily = (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY);
    std::vector<uint8_t> customSizes;
    if (customFamily) {
      customSizes = crosspoint::fonts::CustomBinFontManager::instance().installedSizesFor(SETTINGS.customFontName);
    }
    const int optionCount = customFamily ? static_cast<int>(customSizes.size())
                                         : CrossPointSettings::fontSizeOptionCount(SETTINGS.fontFamily);
    const auto sizeAtIndex = [&](int i) -> uint8_t {
      if (customFamily) return i >= 0 && i < static_cast<int>(customSizes.size()) ? customSizes[i] : 0;
      return CrossPointSettings::displayIndexToFontSize(SETTINGS.fontFamily, i);
    };
    const int textH = renderer.getTextHeight(UI_12_FONT_ID);
    constexpr int kItemPadH = 4;   // horizontal padding inside each chip
    constexpr int kItemPadV = 3;   // vertical padding inside selected chip
    constexpr int kItemGap = 10;   // gap between items
    constexpr int kPopupPad = 16;  // padding inside popup

    // Pre-measure each chip's width — reused for both the row-fitting
    // pass and the draw pass. Custom families can install up to 16
    // sizes; without wrap they ran off both edges of the screen at
    // small font sizes (popupW was capped to the screen but curX kept
    // advancing past it).
    std::vector<std::string> labels(optionCount);
    std::vector<int> chipWidths(optionCount);
    int singleRowW = 0;
    for (int i = 0; i < optionCount; i++) {
      const uint8_t fs = sizeAtIndex(i);
      labels[i] = fontSizeValueLabel(SETTINGS.fontFamily, fs);
      const int labelW = renderer.getTextWidth(UI_12_FONT_ID, labels[i].c_str());
      chipWidths[i] = labelW + kItemPadH * 2;
      singleRowW += chipWidths[i];
    }
    singleRowW += kItemGap * std::max(0, optionCount - 1);

    const char* title = tr(STR_FONT_SIZE);
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, title);
    const int popupW = std::min(pageWidth - 20, std::max(singleRowW, titleW) + kPopupPad * 2);
    const int rowMaxW = popupW - kPopupPad * 2;

    // Pack chips into rows that fit rowMaxW, with a hard cap of
    // kMaxChipsPerRow per row so the popup stays readable on narrow
    // screens even when individual chips are tiny.
    constexpr int kMaxChipsPerRow = 8;
    std::vector<int> rowStarts;
    std::vector<int> rowWidths;
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
        const int chipW = chipWidths[i];
        const bool isSelected = (i == static_cast<int>(fontSizeEditDraftIndex));
        if (isSelected) {
          renderer.fillRect(curX, rowY, chipW, textH + kItemPadV * 2, true);
        }
        renderer.drawText(UI_12_FONT_ID, curX + kItemPadH, rowY + kItemPadV, labels[i].c_str(), !isSelected);
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

      renderer.drawText(UI_10_FONT_ID, popupX + (popupW - titleW) / 2, popupY + 8, settingLabel, true);

      renderer.drawText(UI_12_FONT_ID, popupX + (popupW - valueW) / 2, popupY + 30, valueText.c_str(), true);

      const int barX = popupX + kPopupPad;
      const int barY = popupY + popupH - 22;
      const int barW = popupW - kPopupPad * 2;
      const int barH = 8;
      renderer.drawRect(barX, barY, barW, barH, true);
      const int range = std::max(1, static_cast<int>(valueEditMax) - static_cast<int>(valueEditMin));
      const int filledW =
          2 + ((static_cast<int>(valueEditDraft) - static_cast<int>(valueEditMin)) * std::max(1, barW - 4)) / range;
      renderer.fillRect(barX + 2, barY + 2, filledW, std::max(1, barH - 4), true);
    }
  }

  renderer.displayBuffer();
}
