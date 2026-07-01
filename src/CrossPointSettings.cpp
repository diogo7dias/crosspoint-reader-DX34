#include "CrossPointSettings.h"

#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

#include "Paths.h"
#include "fontIds.h"
#include "persist/CrossPointSettingsJson.h"
#include "persist/PersistManager.h"
#include "persist/SettingsStore.h"
#include "util/StringUtils.h"

CrossPointSettings& CrossPointSettings::getInstance() {
  // The store owns the canonical instance. unsafeMut() preserves the
  // direct-field-write idiom that the existing accessors rely on
  // (`SETTINGS.fontFamily = X` flows through to data_ in the store).
  // Mutations don't auto-mark-dirty here; saveToFile() (now flushSoon)
  // is the explicit dirty trigger, matching pre-RFC #147 semantics.
  return crosspoint::persist::settingsStore().unsafeMut();
}

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

bool CrossPointSettings::saveToFile() const {
  Storage.mkdir(Paths::kDataDir);
  // Mirror the toggle into BitmapHelpers so an in-UI flip takes effect on the
  // very next render; loadFromFile syncs the same global on cold start.
  setBitmapHelpersUseFactoryLUT(useFactoryLUT != 0);
  // Pre-RFC #147 callers expected synchronous I/O. Now: schedule a
  // debounced flush; PersistManager().flushAll() drains us at activity
  // transitions and pre-deep-sleep — the windows where durability
  // genuinely matters. Battery-yank within the debounce window can lose
  // a toggle, same risk APP_STATE has carried since RFC #20.
  crosspoint::persist::settingsStore().flushSoon();
  return true;
}

bool CrossPointSettings::loadFromFile() {
  // Stage-1 retains the legacy binary-format bootstrap as a one-shot
  // prequel: if no JSON exists, walk the binary fallback chain and write
  // a fresh JSON file via the legacy path. The store load below then
  // picks that JSON up. Stage-2 (RFC #147) folds this into a versioned
  // migration chain; for now the conservative compat path stays.
  if (!Storage.exists(SETTINGS_FILE_JSON)) {
    if (Storage.exists(SETTINGS_FILE_BIN)) {
      if (loadFromBinaryFile()) {
        if (JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON)) {
          Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
          LOG_DBG("CPS", "Migrated settings.bin to settings.json");
        } else {
          LOG_ERR("CPS", "Failed to save migrated settings to JSON");
        }
      }
    } else if (Storage.exists(SETTINGS_FILE_BAK)) {
      Storage.rename(SETTINGS_FILE_BAK, SETTINGS_FILE_BIN);
      if (loadFromBinaryFile() && JsonSettingsIO::saveSettings(*this, SETTINGS_FILE_JSON)) {
        Storage.rename(SETTINGS_FILE_BIN, SETTINGS_FILE_BAK);
        LOG_DBG("CPS", "Recovered settings from settings.bin.bak");
      }
    }
  }

  auto& store = crosspoint::persist::settingsStore();
  const auto report = store.load();
  // The deserializer (CrossPointSettingsJson.cpp) sets a sticky flag when
  // JsonSettingsIO::loadSettings reports needsResave. Touch the store so
  // the upgraded payload rewrites on the next tickPersist.
  if (crosspoint::persist::consumeSettingsResaveSticky()) {
    store.touch();
  }
  switch (report.status) {
    case crosspoint::persist::LoadReport::kOk:
      LOG_DBG("CPS", "settings load: ok");
      return true;
    case crosspoint::persist::LoadReport::kRecoveredFromBak:
      LOG_INF("CPS", "settings load: recovered from .bak");
      store.touch();
      return true;
    case crosspoint::persist::LoadReport::kCorrupt:
      LOG_ERR("CPS", "settings load: corrupt; defaults used");
      return false;
    case crosspoint::persist::LoadReport::kMissing:
      LOG_DBG("CPS", "settings load: missing; defaults used");
      return false;
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
      StringUtils::safeStrncpy(opdsServerUrl, urlStr.c_str());
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
      StringUtils::safeStrncpy(opdsUsername, usernameStr.c_str());
    }
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      StringUtils::safeStrncpy(opdsPassword, passwordStr.c_str());
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
    {
      // Legacy global readerBoldSwap: the preference is now stored per book in
      // RecentBooksStore, so read and discard to keep binary offsets aligned
      // for any remaining fields below.
      uint8_t legacyReaderBoldSwap = 0;
      serialization::readPod(inputFile, legacyReaderBoldSwap);
    }
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
  textRenderMode = TEXT_RENDER_NORMAL;
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
    case SLEEP_NEVER:
      return 0;  // 0 = never auto-sleep (guarded at the trigger)
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
    case REFRESH_NEVER:
      return INT_MAX;
  }
}

uint8_t CrossPointSettings::fontFamilyToDisplayIndex(const uint8_t family) {
  // Lector picker order: Bookerly, Georgia, Helvetica, Verdana, Merriweather.
  // Keep in sync with the three StrId label lists (SettingsList, ReaderSettings,
  // FontFamilyPicker) and displayIndexToFontFamily.
  switch (normalizeFontFamily(family)) {
    case BOOKERLY:
      return 0;
    case GEORGIA:
      return 1;
    case HELVETICA:
      return 2;
    case VERDANA:
      return 3;
    case MERRIWEATHER:
      return 4;
    default:
      return 0;  // removed families fold to Bookerly
  }
}

uint8_t CrossPointSettings::displayIndexToFontFamily(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 0:
      return BOOKERLY;
    case 1:
      return GEORGIA;
    case 2:
      return HELVETICA;
    case 3:
      return VERDANA;
    case 4:
      return MERRIWEATHER;
    default:
      return BOOKERLY;
  }
}

uint8_t CrossPointSettings::nextFontSize(const uint8_t family, const uint8_t fontSize) {
  const uint8_t displayIndex = fontSizeToDisplayIndex(family, fontSize);
  const uint8_t optionCount = fontSizeOptionCount(family);
  return displayIndexToFontSize(family, (displayIndex + 1) % optionCount);
}

uint8_t CrossPointSettings::fontSizeToPointSize(const uint8_t family, const uint8_t fontSize) {
  switch (normalizeFontSizeForFamily(family, fontSize)) {
    case SIZE_10:
      return 10;
    case SIZE_11:
      return 11;
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
    case SIZE_18:
      return 18;
    case LARGE:
    default:
      return 17;
  }
}

// Mirror of the build-regime flag in CrossPointSettingsLogic.cpp (internal
// linkage; this TU is not host-tested so it reads the macro directly). True =
// SD build's broad 9-size set {10..18}; otherwise an extra-size family uses the
// flash-extra 7-size set {11..17}. See familyHasExtraSizes() for the gate.
#ifdef CROSSPOINT_SD_FONTS
static constexpr bool kSdExtraSizes = true;
#else
static constexpr bool kSdExtraSizes = false;
#endif

uint8_t CrossPointSettings::fontSizeOptionCount(const uint8_t family) {
  // No extra sizes -> base 5-size flash set {10,12,14,16,17} (ChareInk + plain
  // default). Extra-size families: 9 sizes {10..18} in SD builds, 7 sizes
  // {11..17} in the flash-extra build.
  if (!familyHasExtraSizes(family)) {
    return 5;
  }
  return kSdExtraSizes ? 9 : 7;
}

uint8_t CrossPointSettings::fontSizeToDisplayIndex(const uint8_t family, const uint8_t fontSize) {
  const uint8_t normalized = normalizeFontSizeForFamily(family, fontSize);
  if (familyHasExtraSizes(family)) {
    if (kSdExtraSizes) {
      // 9-size set {10,11,12,13,14,15,16,17,18}.
      switch (normalized) {
        case SIZE_10:
          return 0;
        case SIZE_11:
          return 1;
        case SIZE_12:
          return 2;
        case SIZE_13:
          return 3;
        case SIZE_14:
          return 4;
        case SIZE_15:
          return 5;
        case SIZE_16:
          return 6;
        case SIZE_18:
          return 8;
        case LARGE:
        default:
          return 7;  // 17pt
      }
    }
    // Flash-extra 7-size set {11,12,13,14,15,16,17}. normalize already folded
    // 10 -> 12 and 18 -> 17.
    switch (normalized) {
      case SIZE_11:
        return 0;
      case SIZE_12:
        return 1;
      case SIZE_13:
        return 2;
      case SIZE_14:
        return 3;
      case SIZE_15:
        return 4;
      case SIZE_16:
        return 5;
      case LARGE:
      default:
        return 6;  // 17pt
    }
  }
  // 5-size set: normalize already folded 11/13/15/18 to a kept size.
  switch (normalized) {
    case SIZE_10:
      return 0;
    case SIZE_12:
      return 1;
    case SIZE_14:
      return 2;
    case SIZE_16:
      return 3;
    case LARGE:
    default:
      return 4;
  }
}

uint8_t CrossPointSettings::displayIndexToFontSize(const uint8_t family, const uint8_t displayIndex) {
  if (familyHasExtraSizes(family)) {
    if (kSdExtraSizes) {
      // 9-size set {10,11,12,13,14,15,16,17,18}.
      switch (displayIndex) {
        case 0:
          return SIZE_10;
        case 1:
          return SIZE_11;
        case 2:
          return SIZE_12;
        case 3:
          return SIZE_13;
        case 4:
          return SIZE_14;
        case 5:
          return SIZE_15;
        case 6:
          return SIZE_16;
        case 8:
          return SIZE_18;
        case 7:
        default:
          return LARGE;  // 17pt
      }
    }
    // Flash-extra 7-size set {11,12,13,14,15,16,17}.
    switch (displayIndex) {
      case 0:
        return SIZE_11;
      case 1:
        return SIZE_12;
      case 2:
        return SIZE_13;
      case 3:
        return SIZE_14;
      case 4:
        return SIZE_15;
      case 5:
        return SIZE_16;
      case 6:
      default:
        return LARGE;  // 17pt
    }
  }
  switch (displayIndex) {
    case 0:
      return SIZE_10;
    case 1:
      return SIZE_12;
    case 2:
      return SIZE_14;
    case 3:
      return SIZE_16;
    case 4:
    default:
      return LARGE;  // 17pt
  }
}

int CrossPointSettings::wordSpacingSettingToPixelDelta(const uint8_t mode, const int baseSpaceWidth) {
  switch (mode) {
    case WORD_SPACING_TIGHT:  // -30%
      return -(baseSpaceWidth * 3 / 10);
    case WORD_SPACING_P40:  // +40%
      return (baseSpaceWidth * 2 / 5);
    case WORD_SPACING_WIDE:  // +80%
      return (baseSpaceWidth * 4 / 5);
    case WORD_SPACING_P115:  // +115%
      return (baseSpaceWidth * 23 / 20);
    case WORD_SPACING_VERY_WIDE:  // +150%
      return (baseSpaceWidth * 3 / 2);
    case WORD_SPACING_P195:  // +195%
      return (baseSpaceWidth * 39 / 20);
    case WORD_SPACING_EXTRA_WIDE:  // +240%
      return (baseSpaceWidth * 12 / 5);
    case WORD_SPACING_P300:  // +300%
      return (baseSpaceWidth * 3);
    case WORD_SPACING_NORMAL:  // 0%
    default:
      return 0;
  }
}

int CrossPointSettings::getReaderFontId() const {
  // Emergency render-degrade: a mid-render OOM on a fragmented heap latches this
  // so layout AND render (both resolve their font through here) drop to the
  // smallest built-in font, whose glyph groups fit the largest free block. The
  // latch is transient and never persisted, so the user's real font returns on
  // the next open. See EpubReaderActivity render-OOM recovery + onExit clear.
  if (emergencyRenderFontDowngrade) {
    return BOOKERLY_11_FONT_ID;  // Lector: smallest shipped font = OOM-degrade floor (was ChareInk 12).
  }
  const uint8_t normalizedFontSize = normalizeFontSizeForFamily(fontFamily, fontSize);
  const uint8_t normalizedFamily = normalizeFontFamily(fontFamily);
  if (normalizedFamily == BOOKERLY) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return BOOKERLY_10_FONT_ID;
      case SIZE_12:
        return BOOKERLY_12_FONT_ID;
      case SIZE_14:
        return BOOKERLY_14_FONT_ID;
      case SIZE_16:
        return BOOKERLY_16_FONT_ID;
      case SIZE_11:
        return BOOKERLY_11_FONT_ID;
      case SIZE_13:
        return BOOKERLY_13_FONT_ID;
      case SIZE_15:
        return BOOKERLY_15_FONT_ID;
      case SIZE_18:
        return BOOKERLY_18_FONT_ID;
      case LARGE:
      default:
        return BOOKERLY_17_FONT_ID;
    }
  }
  if (normalizedFamily == GEORGIA) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return GEORGIA_10_FONT_ID;
      case SIZE_12:
        return GEORGIA_12_FONT_ID;
      case SIZE_14:
        return GEORGIA_14_FONT_ID;
      case SIZE_16:
        return GEORGIA_16_FONT_ID;
      case SIZE_11:
        return GEORGIA_11_FONT_ID;
      case SIZE_13:
        return GEORGIA_13_FONT_ID;
      case SIZE_15:
        return GEORGIA_15_FONT_ID;
      case SIZE_18:
        return GEORGIA_18_FONT_ID;
      case LARGE:
      default:
        return GEORGIA_17_FONT_ID;
    }
  }
  // Lato removed (Lector) — folds to Bookerly via normalizeFontFamily.
  if (normalizedFamily == HELVETICA) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return HELVETICA_10_FONT_ID;
      case SIZE_12:
        return HELVETICA_12_FONT_ID;
      case SIZE_14:
        return HELVETICA_14_FONT_ID;
      case SIZE_16:
        return HELVETICA_16_FONT_ID;
      case SIZE_11:
        return HELVETICA_11_FONT_ID;
      case SIZE_13:
        return HELVETICA_13_FONT_ID;
      case SIZE_15:
        return HELVETICA_15_FONT_ID;
      case SIZE_18:
        return HELVETICA_18_FONT_ID;
      case LARGE:
      default:
        return HELVETICA_17_FONT_ID;
    }
  }
  if (normalizedFamily == VERDANA) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return VERDANA_10_FONT_ID;
      case SIZE_12:
        return VERDANA_12_FONT_ID;
      case SIZE_14:
        return VERDANA_14_FONT_ID;
      case SIZE_16:
        return VERDANA_16_FONT_ID;
      case SIZE_11:
        return VERDANA_11_FONT_ID;
      case SIZE_13:
        return VERDANA_13_FONT_ID;
      case SIZE_15:
        return VERDANA_15_FONT_ID;
      case SIZE_18:
        return VERDANA_18_FONT_ID;
      case LARGE:
      default:
        return VERDANA_17_FONT_ID;
    }
  }
  // Lector: Merriweather is a baked flash family (sizes 11..17). Playfair/Galmuri/
  // Vollkorn remain SD-only below (unreachable in the flash build — those families
  // fold to Bookerly via normalizeFontFamily).
  if (normalizedFamily == MERRIWEATHER) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return MERRIWEATHER_10_FONT_ID;
      case SIZE_12:
        return MERRIWEATHER_12_FONT_ID;
      case SIZE_14:
        return MERRIWEATHER_14_FONT_ID;
      case SIZE_16:
        return MERRIWEATHER_16_FONT_ID;
      case SIZE_11:
        return MERRIWEATHER_11_FONT_ID;
      case SIZE_13:
        return MERRIWEATHER_13_FONT_ID;
      case SIZE_15:
        return MERRIWEATHER_15_FONT_ID;
      case SIZE_18:
        return MERRIWEATHER_18_FONT_ID;
      case LARGE:
      default:
        return MERRIWEATHER_17_FONT_ID;
    }
  }
#ifdef CROSSPOINT_SD_FONTS
  if (normalizedFamily == PLAYFAIR) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return PLAYFAIR_10_FONT_ID;
      case SIZE_12:
        return PLAYFAIR_12_FONT_ID;
      case SIZE_14:
        return PLAYFAIR_14_FONT_ID;
      case SIZE_16:
        return PLAYFAIR_16_FONT_ID;
      case SIZE_11:
        return PLAYFAIR_11_FONT_ID;
      case SIZE_13:
        return PLAYFAIR_13_FONT_ID;
      case SIZE_15:
        return PLAYFAIR_15_FONT_ID;
      case SIZE_18:
        return PLAYFAIR_18_FONT_ID;
      case LARGE:
      default:
        return PLAYFAIR_17_FONT_ID;
    }
  }
  if (normalizedFamily == GALMURI) {
    // Pixel font: only two native crisp sizes. Map the lower half of the 10..18
    // scale (10..15) to the 14px (1x) cut and the upper half (16..18) to 28px (2x).
    switch (normalizedFontSize) {
      case SIZE_16:
      case SIZE_18:
      case LARGE:  // 17pt
        return GALMURI_28_FONT_ID;
      default:
        return GALMURI_14_FONT_ID;
    }
  }
  if (normalizedFamily == VOLLKORN) {
    switch (normalizedFontSize) {
      case SIZE_10:
        return VOLLKORN_10_FONT_ID;
      case SIZE_12:
        return VOLLKORN_12_FONT_ID;
      case SIZE_14:
        return VOLLKORN_14_FONT_ID;
      case SIZE_16:
        return VOLLKORN_16_FONT_ID;
      case SIZE_11:
        return VOLLKORN_11_FONT_ID;
      case SIZE_13:
        return VOLLKORN_13_FONT_ID;
      case SIZE_15:
        return VOLLKORN_15_FONT_ID;
      case SIZE_18:
        return VOLLKORN_18_FONT_ID;
      case LARGE:
      default:
        return VOLLKORN_17_FONT_ID;
    }
  }
#endif  // CROSSPOINT_SD_FONTS
  // Lector fallback: any unmatched family folds to Bookerly (also the
  // normalizeFontFamily default), so no path can return a removed ChareInk id.
  switch (normalizedFontSize) {
    case SIZE_12:
      return BOOKERLY_12_FONT_ID;
    case SIZE_14:
      return BOOKERLY_14_FONT_ID;
    case SIZE_16:
      return BOOKERLY_16_FONT_ID;
    case SIZE_11:
      return BOOKERLY_11_FONT_ID;
    case SIZE_13:
      return BOOKERLY_13_FONT_ID;
    case SIZE_15:
      return BOOKERLY_15_FONT_ID;
    case LARGE:
    default:
      return BOOKERLY_17_FONT_ID;
  }
}

int CrossPointSettings::getStatusBarProgressBarHeight() const {
  return statusBarBarThickness == STATUS_BAR_THICKNESS_DOUBLE ? 12 : 6;
}

int CrossPointSettings::getStatusBarFontId() const {
  // Status-bar font size is not user-configurable: always use the larger (clear)
  // UI font. The small option was hard to read and was removed entirely.
  return UI_10_FONT_ID;
}
