// Pure, host-buildable settings logic extracted from CrossPointSettings.cpp.
// These are the Hal-free normalization/migration/validation helpers — moved
// here VERBATIM so they can be compiled and unit-tested on the host (alongside
// the JSON codec in persist/SettingsCodec.cpp) without dragging in HalStorage,
// Logging, BitmapHelpers, Serialization, or fontIds. No behaviour change.
#include <cstdint>

#include "CrossPointSettings.h"

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

uint8_t CrossPointSettings::normalizeFontFamily(const uint8_t family) {
  switch (family) {
    case BOOKERLY:
      return BOOKERLY;
    case GEORGIA:
      return GEORGIA;
    case HELVETICA:
      return HELVETICA;
    case VERDANA:
      return VERDANA;
#if defined(CROSSPOINT_SD_FONTS) || defined(CROSSPOINT_FLASH_EXTRA_SIZES)
    // Lector: Merriweather is baked into flash (5th reader family) in the
    // flash-extra build, and remains available in SD builds.
    case MERRIWEATHER:
      return MERRIWEATHER;
#endif
#ifdef CROSSPOINT_SD_FONTS
    // Playfair/Galmuri/Vollkorn stay SD-only; in the flash build they fold to
    // Bookerly below (never selected because the picker omits them).
    case PLAYFAIR:
      return PLAYFAIR;
    case GALMURI:
      return GALMURI;
    case VOLLKORN:
      return VOLLKORN;
#endif
    // Lector: ChareInk (removed), Lato (removed as a reader font), and every other
    // legacy value collapse to BOOKERLY — the new default — so old settings.json
    // migrates onto a font that is actually compiled in.
    case CHAREINK:
    case LATO:
    default:
      return BOOKERLY;
  }
}

// The 5 offloadable reader families (Bookerly/Georgia/Helvetica/Verdana/Lato) gain
// in-between sizes as Tier-1 fonts (tables in flash; bitmaps streamed from SD in SD
// builds, baked into flash in the flash-extra build — zero added heap either way).
// Two extra-size regimes:
//   * SD builds (CROSSPOINT_SD_FONTS): the broad 9-size set {10,11,12,13,14,15,16,
//     17,18}, shared by these 5 + the SD-only families (Merriweather/Playfair/
//     Galmuri/Vollkorn).
//   * Flash-extra build (CROSSPOINT_FLASH_EXTRA_SIZES, no SD): the 7-size set
//     {11,12,13,14,15,16,17} for the 5 families (10 and 18 are NOT baked).
// ChareInk is always excluded — it is the in-flash fallback floor and ships only
// the five base sizes {10,12,14,16,17}. In the plain default build (neither flag)
// or for ChareInk, the extra sizes fold to the nearest kept size so a persisted
// value never lands on a font that does not exist.
#ifdef CROSSPOINT_SD_FONTS
static constexpr bool kSdExtraSizes = true;
#else
static constexpr bool kSdExtraSizes = false;
#endif
// Flash-extra only applies when SD is NOT in play (SD's broader set wins if both
// are somehow defined), so the two regimes never overlap.
#if defined(CROSSPOINT_FLASH_EXTRA_SIZES) && !defined(CROSSPOINT_SD_FONTS)
static constexpr bool kFlashExtraSizes = true;
#else
static constexpr bool kFlashExtraSizes = false;
#endif

bool CrossPointSettings::familyHasExtraSizes(const uint8_t family) {
  return (kSdExtraSizes || kFlashExtraSizes) && normalizeFontFamily(family) != CHAREINK;
}

uint8_t CrossPointSettings::normalizeFontSizeForFamily(const uint8_t family, const uint8_t fontSize) {
  const bool extra = familyHasExtraSizes(family);
  // Flash-extra families ship the 7-size set {11..17}: sizes 10 and 18 have no
  // baked bitmaps there, so fold them to a kept neighbour. SD's 9-size set keeps
  // both; the plain default 5-size set keeps 10 (its smallest) and folds 18 -> 17.
  const bool flashSevenSize = extra && !kSdExtraSizes;
  switch (fontSize) {
    case SIZE_10:
      return flashSevenSize ? SIZE_12 : SIZE_10;  // flash-extra drops 10 -> 12
    case SIZE_12:
      return SIZE_12;
    case SIZE_14:
      return SIZE_14;
    case SIZE_16:
      return SIZE_16;
    case LARGE:
      return SIZE_16;  // 17 removed -> 16
    case SIZE_11:
      return extra ? SIZE_11 : SIZE_12;  // extra: real 11; else -> 12
    case SIZE_13:
      return extra ? SIZE_13 : SIZE_14;  // extra: real 13; else -> 14
    case SIZE_15:
      return extra ? SIZE_15 : SIZE_16;  // extra: real 15; else -> 16
    case SIZE_18:
      return (extra && kSdExtraSizes) ? SIZE_18 : SIZE_16;  // SD-only real 18; else -> 16
    case MEDIUM:
      return SIZE_14;  // legacy 15pt MEDIUM -> 14 (preserve existing user size)
    case X_LARGE:
    default:
      return SIZE_16;  // legacy -> 16
  }
}
