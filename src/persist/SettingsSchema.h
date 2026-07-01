#pragma once
/**
 * @file SettingsSchema.h
 * @brief Shared descriptor table of the MECHANICAL settings fields.
 *
 * A field is MECHANICAL iff its persistence is exactly one of:
 *   - enum  : encode `doc[key] = s.member;`
 *             decode `s.member = clampEnum(doc[key] | deflt, count, deflt);`
 *   - raw   : encode `doc[key] = s.member;`
 *             decode `s.member = doc[key] | deflt;`  then optional
 *                    `if (s.member > maxRaw) s.member = deflt;`
 *
 * encodeSettings() and decodeSettings() (SettingsCodec.cpp) both drive these
 * fields off this ONE table, so a mechanical field can never again be
 * written-but-not-read (the v6.0.0 granular-status-bar bug class). Irregular
 * fields (migrations, sentinels, gates, env deps, cross-field normalize, char
 * arrays, forced values) stay hand-coded in the codec.
 *
 * The rows are listed in the EXACT order encodeSettings emits them, so the
 * encode walk reproduces the current settings.json key order (the GOLDEN
 * snapshot test enforces this). Where irregular fields interleave, the codec
 * emits this table in ordered slices around the explicit irregular emits.
 *
 * Header-only POD table: constexpr, no heap, no std::function. Included by
 * SettingsCodec.cpp and by the host structural test.
 */
#include <cstddef>
#include <cstdint>

#include "../CrossPointSettings.h"

namespace crosspoint {
namespace persist {

// One mechanical settings field. POD; safe in a constexpr aggregate.
struct FieldDesc {
  const char* key;                       // settings.json key (string literal, static lifetime)
  uint8_t CrossPointSettings::* member;  // pointer-to-member of the (uint8_t) field
  uint8_t deflt;                         // default applied via ArduinoJson's `| deflt`
  uint8_t count;                         // >0 => clampEnum to [0,count); 0 => raw/bool field
  uint8_t maxRaw;                        // count==0 only: >0 => clamp `if (v > maxRaw) v = deflt`; 0 => no clamp
};

using SS = CrossPointSettings;

// Mechanical fields IN ENCODE ORDER. See SettingsCodec.cpp for the irregular
// fields that interleave between these rows.
inline constexpr FieldDesc kFields[] = {
    // --- run A (top of doc, before the gated status-bar block) ---
    {"homeLayout", &SS::homeLayout, SS::HOME_LAYOUT_CLASSIC, SS::HOME_LAYOUT_COUNT, 0},
    {"quoteScreenStyle", &SS::quoteScreenStyle, SS::QUOTE_STYLE_CLASSIC, SS::QUOTE_STYLE_COUNT, 0},
    {"sleepScreen", &SS::sleepScreen, SS::DARK, SS::SLEEP_SCREEN_MODE_COUNT, 0},
    {"sleepScreenCoverMode", &SS::sleepScreenCoverMode, SS::FIT, SS::SLEEP_SCREEN_COVER_MODE_COUNT, 0},
    {"sleepScreenCoverFilter", &SS::sleepScreenCoverFilter, SS::NO_FILTER, SS::SLEEP_SCREEN_COVER_FILTER_COUNT, 0},
    {"showSleepImageFilename", &SS::showSleepImageFilename, 0, 0, 0},
    {"showSleepFavoriteBadge", &SS::showSleepFavoriteBadge, 0, 0, 0},
    {"statusBar", &SS::statusBar, SS::FULL, SS::STATUS_BAR_MODE_COUNT, 0},
    // --- run B (after extraParagraph + wordSpacing irregular block) ---
    {"firstLineIndentMode", &SS::firstLineIndentMode, SS::INDENT_BOOK, SS::FIRST_LINE_INDENT_MODE_COUNT, 0},
    // --- run C (after readerStyle/textRender/useFactoryLUT irregular block) ---
    {"shortPwrBtn", &SS::shortPwrBtn, SS::IGNORE, SS::SHORT_PWRBTN_COUNT, 0},
    {"orientation", &SS::orientation, SS::PORTRAIT, SS::ORIENTATION_COUNT, 0},
    {"tiltPageTurn", &SS::tiltPageTurn, SS::TILT_OFF, SS::TILT_PAGE_TURN_COUNT, 0},
    // --- run D (between statusBarClock and clockFormat irregular emits) ---
    // Biased quarter-hour UTC offset, clamped to [-12:00 .. +14:00] (0..104).
    {"clockUtcOffsetQ", &SS::clockUtcOffsetQ, 48, 105, 0},
    // --- run E (after clockFormat/clockHasBeenSynced irregular emits) ---
    {"sideButtonLayout", &SS::sideButtonLayout, SS::PREV_NEXT, SS::SIDE_BUTTON_LAYOUT_COUNT, 0},
    // --- run F (after front-button/font/lineSpacing irregular block) ---
    {"paragraphAlignment", &SS::paragraphAlignment, SS::JUSTIFIED, SS::PARAGRAPH_ALIGNMENT_COUNT, 0},
    {"sleepTimeout", &SS::sleepTimeout, SS::SLEEP_10_MIN, SS::SLEEP_TIMEOUT_COUNT, 0},
    {"showHiddenFiles", &SS::showHiddenFiles, 0, 0, 0},
    {"randomBookOnBoot", &SS::randomBookOnBoot, 0, 0, 0},
    {"refreshFrequency", &SS::refreshFrequency, SS::REFRESH_15, SS::REFRESH_FREQUENCY_COUNT, 0},
    // --- run G (after screenMargin irregular emit) ---
    {"uniformMargins", &SS::uniformMargins, 0, 0, 1},
    // --- run H (after dynamicMargins/split-margin irregular block) ---
    {"hideBatteryPercentage", &SS::hideBatteryPercentage, SS::HIDE_NEVER, SS::HIDE_BATTERY_PERCENTAGE_COUNT, 0},
    {"longPressChapterSkip", &SS::longPressChapterSkip, 1, 0, 0},
    {"hyphenationEnabled", &SS::hyphenationEnabled, 0, 0, 0},
    // --- run I (after uiLanguage irregular emit) ---
    {"fadingFix", &SS::fadingFix, 0, 0, 0},
    // --- run J (after embeddedStyle irregular emit; tail of doc) ---
    {"debugBorders", &SS::debugBorders, 0, 0, 0},
    {"highlightMode", &SS::highlightMode, 0, SS::HIGHLIGHT_MODE_COUNT, 0},
    {"darkMode", &SS::darkMode, 0, 0, 1},
    {"booksFolderOrder", &SS::booksFolderOrder, 0, 0, 1},
    {"imageDither", &SS::imageDither, SS::IMAGE_DITHER_QUALITY, SS::IMAGE_DITHER_COUNT, 0},
};

inline constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);

}  // namespace persist
}  // namespace crosspoint
