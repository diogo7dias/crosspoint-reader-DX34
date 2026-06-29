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
    case LATO:
      return LATO;
    case HELVETICA:
      return HELVETICA;
    case VERDANA:
      return VERDANA;
#ifdef CROSSPOINT_SD_FONTS
    // Merriweather + Playfair are SD-only families (no in-flash fallback). They
    // exist only in CROSSPOINT_SD_FONTS builds; in the default build a persisted
    // value falls through to CHAREINK below, so old settings never select a font
    // that was not compiled in.
    case MERRIWEATHER:
      return MERRIWEATHER;
    case PLAYFAIR:
      return PLAYFAIR;
    case GALMURI:
      return GALMURI;
    case VOLLKORN:
      return VOLLKORN;
#endif
    // Removed families (VOLLKORN 2, GALMURI 9, TT2020 10, BITTER 11, CUSTOM 12,
    // F25_BANK_PRINTER 13, PIXEL32 15, ETBB 16, ROSARIVO 17, COZETTE 19) and any
    // other legacy value collapse to CHAREINK so old settings.json migrates. A
    // user who had a removed font selected lands on ChareInk.
    case CHAREINK:
    default:
      return CHAREINK;
  }
}

// With SD fonts enabled, every offloadable family gains the in-between/large
// sizes 11/13/15/18 as Tier-1 fonts (tables in flash, bitmaps streamed from SD
// packs — zero added heap, same as the existing sizes). ChareInk is excluded: it
// is the always-in-flash fallback floor and ships only the five flash sizes. In
// the default (non-SD) build, or for ChareInk, the extra sizes fold to the
// nearest kept size so a persisted value never lands on a font that does not
// exist.
#ifdef CROSSPOINT_SD_FONTS
static constexpr bool kSdExtraSizes = true;
#else
static constexpr bool kSdExtraSizes = false;
#endif

bool CrossPointSettings::familyHasExtraSizes(const uint8_t family) {
  return kSdExtraSizes && normalizeFontFamily(family) != CHAREINK;
}

uint8_t CrossPointSettings::normalizeFontSizeForFamily(const uint8_t family, const uint8_t fontSize) {
  const bool extra = familyHasExtraSizes(family);
  switch (fontSize) {
    case SIZE_10:
      return SIZE_10;
    case SIZE_12:
      return SIZE_12;
    case SIZE_14:
      return SIZE_14;
    case SIZE_16:
      return SIZE_16;
    case LARGE:
      return LARGE;  // 17pt
    case SIZE_11:
      return extra ? SIZE_11 : SIZE_12;  // SD: real 11; else -> 12
    case SIZE_13:
      return extra ? SIZE_13 : SIZE_14;  // SD: real 13; else -> 14
    case SIZE_15:
      return extra ? SIZE_15 : SIZE_16;  // SD: real 15; else -> 16
    case SIZE_18:
      return extra ? SIZE_18 : LARGE;  // SD: real 18; else -> 17
    case MEDIUM:
      return SIZE_14;  // legacy 15pt MEDIUM -> 14 (preserve existing user size)
    case X_LARGE:
    default:
      return LARGE;  // legacy 19 -> 17
  }
}
