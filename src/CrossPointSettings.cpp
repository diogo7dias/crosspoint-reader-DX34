#include "CrossPointSettings.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "Paths.h"
#include "fontIds.h"

// Initialize the static instance
CrossPointSettings CrossPointSettings::instance;

void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);
  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
constexpr uint8_t SETTINGS_FILE_VERSION = 1;
constexpr char SETTINGS_FILE_BIN[] = "/.crosspoint/settings.bin";
constexpr char SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
constexpr char SETTINGS_FILE_BAK[] = "/.crosspoint/settings.bin.bak";

// Validate front button mapping to ensure each hardware button is unique.
// If duplicates are detected, reset to the default physical order to prevent
// invalid mappings.
void validateFrontButtonMapping(CrossPointSettings& settings) {
  // Snapshot the logical->hardware mapping so we can compare for duplicates.
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  for (size_t i = 0; i < 4; i++) {
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        // Duplicate detected: restore the default physical order (Back,
        // Confirm, Left, Right).
        settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
        settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
        settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
        settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

// Convert legacy front button layout into explicit logical->hardware mapping.
void applyLegacyFrontButtonLayout(CrossPointSettings& settings) {
  switch (static_cast<CrossPointSettings::FRONT_BUTTON_LAYOUT>(settings.frontButtonLayout)) {
    case CrossPointSettings::LEFT_RIGHT_BACK_CONFIRM:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_CONFIRM;
      break;
    case CrossPointSettings::LEFT_BACK_CONFIRM_RIGHT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
    case CrossPointSettings::BACK_CONFIRM_RIGHT_LEFT:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_RIGHT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_LEFT;
      break;
    case CrossPointSettings::BACK_CONFIRM_LEFT_RIGHT:
    default:
      settings.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      settings.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      settings.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      settings.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
      break;
  }
}

}  // namespace

void migrateLegacyStatusBarMode(CrossPointSettings& settings) {
  settings.statusBarEnabled = 1;
  settings.statusBarShowBattery = 1;
  settings.statusBarShowPageCounter = 0;
  settings.statusBarPageCounterMode = CrossPointSettings::STATUS_PAGE_CURRENT_OVER_TOTAL;
  settings.statusBarShowBookPercentage = 0;
  settings.statusBarShowChapterPercentage = 0;
  settings.statusBarShowBookBar = 0;
  settings.statusBarShowChapterBar = 0;
  settings.statusBarShowChapterTitle = 1;
  settings.statusBarNoTitleTruncation = 0;
  settings.statusBarTopLine = 0;
  settings.statusBarBatteryPosition = CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT;
  settings.statusBarProgressTextPosition = CrossPointSettings::STATUS_AT_BOTTOM;
  settings.statusBarPageCounterPosition = CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  settings.statusBarBookPercentagePosition = CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  settings.statusBarChapterPercentagePosition = CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  settings.statusBarBookBarPosition = CrossPointSettings::STATUS_AT_BOTTOM;
  settings.statusBarChapterBarPosition = CrossPointSettings::STATUS_AT_BOTTOM;
  settings.statusBarTitlePosition = CrossPointSettings::STATUS_AT_BOTTOM;
  settings.statusBarTextAlignment = CrossPointSettings::STATUS_TEXT_RIGHT;
  settings.statusBarProgressStyle = CrossPointSettings::STATUS_BAR_THICK;

  switch (static_cast<CrossPointSettings::STATUS_BAR_MODE>(settings.statusBar)) {
    case CrossPointSettings::STATUS_BAR_MODE::NONE:
      settings.statusBarEnabled = 0;
      settings.statusBarShowBattery = 0;
      settings.statusBarShowChapterTitle = 0;
      break;
    case CrossPointSettings::STATUS_BAR_MODE::NO_PROGRESS:
      break;
    case CrossPointSettings::STATUS_BAR_MODE::FULL:
      settings.statusBarShowPageCounter = 1;
      settings.statusBarShowBookPercentage = 1;
      break;
    case CrossPointSettings::STATUS_BAR_MODE::BOOK_PROGRESS_BAR:
      settings.statusBarShowPageCounter = 1;
      settings.statusBarShowBookBar = 1;
      break;
    case CrossPointSettings::STATUS_BAR_MODE::ONLY_BOOK_PROGRESS_BAR:
      settings.statusBarShowBattery = 0;
      settings.statusBarShowChapterTitle = 0;
      settings.statusBarShowBookBar = 1;
      break;
    case CrossPointSettings::STATUS_BAR_MODE::CHAPTER_PROGRESS_BAR:
      settings.statusBarShowBookPercentage = 1;
      settings.statusBarShowChapterBar = 1;
      break;
    default:
      break;
  }
}

namespace {  // reopen anonymous namespace for remaining local helpers

uint8_t legacyLineSpacingToPercent(const uint8_t legacy) {
  switch (legacy) {
    case CrossPointSettings::TIGHT:
      return 95;
    case CrossPointSettings::WIDE:
      return 125;
    case CrossPointSettings::NORMAL:
    default:
      return 110;
  }
}
}  // namespace

uint8_t CrossPointSettings::normalizeStatusBarPageCounterMode(uint8_t mode) {
  switch (mode) {
    case STATUS_PAGE_CURRENT_OVER_TOTAL:
      return STATUS_PAGE_CURRENT_OVER_TOTAL;
    case STATUS_PAGE_LEFT_TEXT:
    case 2:
      return STATUS_PAGE_LEFT_TEXT;
    default:
      return STATUS_PAGE_CURRENT_OVER_TOTAL;
  }
}

void CrossPointSettings::validateFrontButtonMapping(CrossPointSettings& settings) {
  const uint8_t mapping[] = {settings.frontButtonBack, settings.frontButtonConfirm, settings.frontButtonLeft,
                             settings.frontButtonRight};
  // Check range validity and duplicates
  for (size_t i = 0; i < 4; i++) {
    if (mapping[i] > FRONT_HW_RIGHT) {
      // Out of range — reset all to defaults
      settings.frontButtonBack = FRONT_HW_BACK;
      settings.frontButtonConfirm = FRONT_HW_CONFIRM;
      settings.frontButtonLeft = FRONT_HW_LEFT;
      settings.frontButtonRight = FRONT_HW_RIGHT;
      return;
    }
    for (size_t j = i + 1; j < 4; j++) {
      if (mapping[i] == mapping[j]) {
        settings.frontButtonBack = FRONT_HW_BACK;
        settings.frontButtonConfirm = FRONT_HW_CONFIRM;
        settings.frontButtonLeft = FRONT_HW_LEFT;
        settings.frontButtonRight = FRONT_HW_RIGHT;
        return;
      }
    }
  }
}

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir(Paths::kDataDir);
  return JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON);
}

bool CrossPointSettings::loadFromFile() {
  // Try JSON first
  String json = JsonSettingsIO::safeReadFile(SETTINGS_FILE_JSON);
  if (!json.isEmpty()) {
    bool resave = false;
    bool result = JsonSettingsIO::loadSettings(*this, json.c_str(), &resave);
    if (result && resave) {
      if (saveToFile()) {
        LOG_DBG("CPS", "Resaved settings to update format");
      } else {
        LOG_ERR("CPS", "Failed to resave settings after format update");
      }
    }
    return result;
  }

  // Fall back to binary migration
  if (Storage.exists(SETTINGS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Migrated settings.bin to settings.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        return false;
      }
    }
  }

  // Fallback to old backed up settings if .json was destroyed in a crash
  if (Storage.exists(SETTINGS_FILE_BAK)) {
    // temporarily swap name and load it as binary
    Storage.rename(SETTINGS_FILE_BAK, SETTINGS_FILE_BIN);
    bool didLoad = loadFromBinaryFile();
    if (didLoad && saveToFile()) {
      Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
      LOG_DBG("CPS", "Recovered settings from settings.bin.bak");
      return true;
    }
  }

  return false;
}

bool CrossPointSettings::loadFromBinaryFile() {
  FsFile inputFile;
  if (!Storage.openFileForRead("CPS", SETTINGS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version != SETTINGS_FILE_VERSION) {
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);
  logSerial.printf("[CPS] loadFromFile: version=%d, count=%d\n", (int)version, (int)fileSettingsCount);

  uint8_t settingsRead = 0;
  bool frontButtonMappingRead = false;
  bool splitReaderMarginsRead = false;
  bool statusBarGranularRead = false;
  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      uint8_t legacyExtraSpacing = 1;
      serialization::readPod(inputFile, legacyExtraSpacing);
      extraParagraphSpacingLevel = legacyExtraSpacing ? EXTRA_SPACING_M : EXTRA_SPACING_OFF;
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLayout,
                    FRONT_BUTTON_LAYOUT_COUNT);  // legacy
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontFamily, FONT_FAMILY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, lineSpacing, LINE_COMPRESSION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMargin);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, longPressChapterSkip);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonBack, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonConfirm, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonLeft, FRONT_BUTTON_HARDWARE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, frontButtonRight, FRONT_BUTTON_HARDWARE_COUNT);
    frontButtonMappingRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, embeddedStyle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMarginHorizontal);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMarginTop);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, screenMarginBottom);
    splitReaderMarginsRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, showSleepImageFilename);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowBattery);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowPageCounter);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowBookPercentage);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowChapterPercentage);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowBookBar);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowChapterBar);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarShowChapterTitle);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarTopLine);
    if (++settingsRead >= fileSettingsCount) break;
    readAndValidate(inputFile, statusBarTextAlignment, STATUS_TEXT_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;
    {
      // Migration: old single statusBarProgressStyle → read and discard
      readAndValidate(inputFile, statusBarProgressStyle, STATUS_BAR_PROGRESS_STYLE_COUNT);
    }
    statusBarGranularRead = true;
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, readerBoldSwap);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, debugBorders);
    if (++settingsRead >= fileSettingsCount) break;
    // New fields added at end for backward compatibility
  } while (false);

  if (frontButtonMappingRead) {
    CrossPointSettings::validateFrontButtonMapping(*this);
  } else {
    applyLegacyFrontButtonLayout(*this);
  }

  // Migration path for older settings files that only had uniform screenMargin.
  if (!splitReaderMarginsRead) {
    screenMarginHorizontal = screenMargin;
    screenMarginTop = screenMargin;
    screenMarginBottom = screenMargin;
  }

  if (!statusBarGranularRead) {
    migrateLegacyStatusBarMode(*this);
  }

  fontFamily = normalizeFontFamily(fontFamily);
  fontSize = normalizeFontSizeForFamily(fontFamily, fontSize);

  // Binary settings only store legacy 3-step line spacing; newer reader
  // spacing/style settings fall back to their current defaults.
  lineSpacingPercent = legacyLineSpacingToPercent(lineSpacing);
  wordSpacingPercent = WORD_SPACING_NORMAL;
  firstLineIndentMode = INDENT_BOOK;
  readerStyleMode = embeddedStyle ? READER_STYLE_HYBRID : READER_STYLE_USER;
  textRenderMode = TEXT_RENDER_CRISP;
  textAntiAliasing = 0;

  inputFile.close();
  LOG_DBG("CPS", "Settings loaded from binary file");
  return true;
}

float CrossPointSettings::getReaderLineCompression() const {
  uint8_t spacing = lineSpacingPercent;
  if (spacing < 35) {
    spacing = 35;
  } else if (spacing > 150) {
    spacing = 150;
  }
  return static_cast<float>(spacing) / 100.0f;
}

unsigned long CrossPointSettings::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int CrossPointSettings::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
}

uint8_t CrossPointSettings::normalizeFontFamily(const uint8_t family) {
  switch (family) {
    case BOOKERLY:
      return BOOKERLY;
    case VOLLKORN:
      return VOLLKORN;
    case IMFELL:
      return IMFELL;
    case CHAREINK:
    default:
      return CHAREINK;
  }
}

uint8_t CrossPointSettings::fontFamilyToDisplayIndex(const uint8_t family) {
  switch (normalizeFontFamily(family)) {
    case BOOKERLY:
      return 1;
    case VOLLKORN:
      return 2;
    case IMFELL:
      return 3;
    case CHAREINK:
    default:
      return 0;
  }
}

uint8_t CrossPointSettings::displayIndexToFontFamily(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return BOOKERLY;
    case 2:
      return VOLLKORN;
    case 3:
      return IMFELL;
    case 0:
    default:
      return CHAREINK;
  }
}

uint8_t CrossPointSettings::normalizeFontSizeForFamily(const uint8_t family, const uint8_t fontSize) {
  // IM Fell ships at size 15 only — any stored size normalizes to SIZE_15.
  if (normalizeFontFamily(family) == IMFELL) {
    return SIZE_15;
  }
  // All other families: active set 12, 13, 14, 15, 16, 17 (LARGE)
  switch (fontSize) {
    case SIZE_12:
      return SIZE_12;
    case SIZE_13:
      return SIZE_13;
    case SIZE_14:
      return SIZE_14;
    case SIZE_15:
      return SIZE_15;
    case SIZE_16:
      return SIZE_16;
    case LARGE:
      return LARGE;  // 17pt
    case SIZE_10:
      return SIZE_12;  // legacy 10 -> 12
    case MEDIUM:
      return SIZE_14;  // legacy 15pt MEDIUM -> 14 (preserve existing user size)
    case SIZE_18:
    case X_LARGE:
    default:
      return LARGE;  // legacy 18/19 -> 17
  }
}

uint8_t CrossPointSettings::nextFontSize(const uint8_t family, const uint8_t fontSize) {
  const uint8_t displayIndex = fontSizeToDisplayIndex(family, fontSize);
  const uint8_t optionCount = fontSizeOptionCount(family);
  return displayIndexToFontSize(family, (displayIndex + 1) % optionCount);
}

uint8_t CrossPointSettings::fontSizeToPointSize(const uint8_t family, const uint8_t fontSize) {
  switch (normalizeFontSizeForFamily(family, fontSize)) {
    case SIZE_12:
      return 12;
    case SIZE_13:
      return 13;
    case SIZE_14:
      return 14;
    case SIZE_15:
      return 15;
    case SIZE_16:
      return 16;
    case LARGE:
    default:
      return 17;
  }
}

uint8_t CrossPointSettings::fontSizeOptionCount(const uint8_t family) {
  if (normalizeFontFamily(family) == IMFELL) {
    return 1;  // IM Fell: 15 only
  }
  return 6;  // 12, 13, 14, 15, 16, 17 — all other families
}

uint8_t CrossPointSettings::fontSizeToDisplayIndex(const uint8_t family, const uint8_t fontSize) {
  if (normalizeFontFamily(family) == IMFELL) {
    return 0;
  }
  switch (normalizeFontSizeForFamily(family, fontSize)) {
    case SIZE_12:
      return 0;
    case SIZE_13:
      return 1;
    case SIZE_14:
      return 2;
    case SIZE_15:
      return 3;
    case SIZE_16:
      return 4;
    case LARGE:
    default:
      return 5;
  }
}

uint8_t CrossPointSettings::displayIndexToFontSize(const uint8_t family, const uint8_t displayIndex) {
  if (normalizeFontFamily(family) == IMFELL) {
    return SIZE_15;
  }
  switch (displayIndex) {
    case 0:
      return SIZE_12;
    case 1:
      return SIZE_13;
    case 2:
      return SIZE_14;
    case 3:
      return SIZE_15;
    case 4:
      return SIZE_16;
    case 5:
    default:
      return LARGE;  // 17pt
  }
}

int CrossPointSettings::wordSpacingSettingToPixelDelta(const uint8_t mode, const int baseSpaceWidth) {
  switch (mode) {
    case WORD_SPACING_TIGHT:
      return -(baseSpaceWidth * 3 / 10);
    case WORD_SPACING_WIDE:
      return (baseSpaceWidth * 4 / 5);
    case WORD_SPACING_VERY_WIDE:
      return (baseSpaceWidth * 3 / 2);
    case WORD_SPACING_EXTRA_WIDE:
      return (baseSpaceWidth * 12 / 5);
    case WORD_SPACING_NORMAL:
    default:
      return 0;
  }
}

int CrossPointSettings::getReaderFontId() const {
  const uint8_t normalizedFontSize = normalizeFontSizeForFamily(fontFamily, fontSize);
  const uint8_t normalizedFamily = normalizeFontFamily(fontFamily);
  if (normalizedFamily == IMFELL) {
    return IMFELL_15_FONT_ID;
  }
  if (normalizedFamily == BOOKERLY) {
    switch (normalizedFontSize) {
      case SIZE_12:
        return BOOKERLY_12_FONT_ID;
      case SIZE_13:
        return BOOKERLY_13_FONT_ID;
      case SIZE_14:
        return BOOKERLY_14_FONT_ID;
      case SIZE_15:
        return BOOKERLY_15_FONT_ID;
      case SIZE_16:
        return BOOKERLY_16_FONT_ID;
      case LARGE:
      default:
        return BOOKERLY_17_FONT_ID;
    }
  }
  if (normalizedFamily == VOLLKORN) {
    switch (normalizedFontSize) {
      case SIZE_12:
        return VOLLKORN_12_FONT_ID;
      case SIZE_13:
        return VOLLKORN_13_FONT_ID;
      case SIZE_14:
        return VOLLKORN_14_FONT_ID;
      case SIZE_15:
        return VOLLKORN_15_FONT_ID;
      case SIZE_16:
        return VOLLKORN_16_FONT_ID;
      case LARGE:
      default:
        return VOLLKORN_17_FONT_ID;
    }
  }
  switch (normalizedFontSize) {
    case SIZE_12:
      return CHAREINK_12_FONT_ID;
    case SIZE_13:
      return CHAREINK_13_FONT_ID;
    case SIZE_14:
      return CHAREINK_14_FONT_ID;
    case SIZE_15:
      return CHAREINK_15_FONT_ID;
    case SIZE_16:
      return CHAREINK_16_FONT_ID;
    case LARGE:
    default:
      return CHAREINK_17_FONT_ID;
  }
}

int CrossPointSettings::getStatusBarProgressBarHeight() const {
  return statusBarBarThickness == STATUS_BAR_THICKNESS_DOUBLE ? 12 : 6;
}

int CrossPointSettings::getStatusBarFontId() const {
  return statusBarFontSize == STATUS_FONT_MEDIUM ? UI_10_FONT_ID : SMALL_FONT_ID;
}
