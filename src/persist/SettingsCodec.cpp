#include "SettingsCodec.h"

#include <ArduinoJson.h>

#include <cstring>
#include <string>

#include "../CrossPointSettings.h"
#include "SettingsSchema.h"

#if defined(UNIT_TEST_HOST)
#define LOG_DIAG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#else
#include <Logging.h>
#endif

namespace {
inline uint8_t clampEnum(uint8_t val, uint8_t maxVal, uint8_t def) { return val < maxVal ? val : def; }
template <size_t N>
inline void copyCStr(char (&dst)[N], const char* src) {
  std::strncpy(dst, src, N - 1);
  dst[N - 1] = '\0';
}
}  // namespace

void crosspoint::persist::encodeSettings(const CrossPointSettings& s, JsonDocument& doc, const SettingsEnv& env) {
  using S = CrossPointSettings;
  // Mechanical fields are emitted from the shared kFields table (SettingsSchema.h)
  // in ordered slices; irregular/companion fields are emitted explicitly at their
  // exact positions between the slices, reproducing the historic key order. The
  // GOLDEN snapshot test pins that order byte-for-byte.
  size_t cur = 0;
  auto emitMechUntil = [&](const char* stopKey) {
    while (cur < kFieldCount && std::strcmp(kFields[cur].key, stopKey) != 0) {
      doc[kFields[cur].key] = s.*(kFields[cur].member);
      ++cur;
    }
  };
  auto emitMechRest = [&]() {
    while (cur < kFieldCount) {
      doc[kFields[cur].key] = s.*(kFields[cur].member);
      ++cur;
    }
  };

  // run A: homeLayout .. statusBar
  emitMechUntil("firstLineIndentMode");

  // --- gated status-bar block (decode is present-or-migrate gated; encode is
  // unconditional). v6.0.0 granular items mirror the reading_themes.json path so
  // toggling them from the Settings menu survives a reboot. ---
  doc["statusBarEnabled"] = s.statusBarEnabled;
  doc["statusBarShowBattery"] = s.statusBarShowBattery;
  doc["statusBarShowPageCounter"] = s.statusBarShowPageCounter;
  doc["statusBarPageCounterMode"] = s.statusBarPageCounterMode;
  doc["statusBarShowBookPercentage"] = s.statusBarShowBookPercentage;
  doc["statusBarShowChapterPercentage"] = s.statusBarShowChapterPercentage;
  doc["statusBarShowBookBar"] = s.statusBarShowBookBar;
  doc["statusBarShowChapterBar"] = s.statusBarShowChapterBar;
  doc["statusBarShowChapterTitle"] = s.statusBarShowChapterTitle;
  doc["statusBarNoTitleTruncation"] = s.statusBarNoTitleTruncation;
  doc["statusBarTopLine"] = s.statusBarTopLine;
  doc["statusBarBatteryPosition"] = s.statusBarBatteryPosition;
  doc["batteryPositionV2"] = true;
  doc["statusBarProgressTextPosition"] = s.statusBarProgressTextPosition;
  doc["statusBarPageCounterPosition"] = s.statusBarPageCounterPosition;
  doc["statusBarBookPercentagePosition"] = s.statusBarBookPercentagePosition;
  doc["statusBarChapterPercentagePosition"] = s.statusBarChapterPercentagePosition;
  doc["statusBarBookBarPosition"] = s.statusBarBookBarPosition;
  doc["statusBarChapterBarPosition"] = s.statusBarChapterBarPosition;
  doc["statusBarTitlePosition"] = s.statusBarTitlePosition;
  doc["statusBarTextAlignment"] = s.statusBarTextAlignment;
  doc["statusBarProgressStyle"] = s.statusBarProgressStyle;
  doc["statusBarBarThickness"] = s.statusBarBarThickness;
  doc["statusBarShowPagesLeft"] = s.statusBarShowPagesLeft;
  doc["statusBarPagesLeftPosition"] = s.statusBarPagesLeftPosition;
  doc["statusBarTitleContent"] = s.statusBarTitleContent;
  doc["statusBarShowChapterNumber"] = s.statusBarShowChapterNumber;
  doc["statusBarChapterNumberPosition"] = s.statusBarChapterNumberPosition;
  doc["statusBarShowQuoteCount"] = s.statusBarShowQuoteCount;
  doc["statusBarQuoteCountPosition"] = s.statusBarQuoteCountPosition;
  doc["statusBarShowFreeHeap"] = s.statusBarShowFreeHeap;
  doc["statusBarFreeHeapPosition"] = s.statusBarFreeHeapPosition;

  // extra-paragraph + word-spacing (migrations + legacy companion keys)
  doc["extraParagraphSpacingLevel"] = s.extraParagraphSpacingLevel;
  // Legacy compatibility key for older builds that still expect a toggle.
  doc["extraParagraphSpacing"] = s.extraParagraphSpacingLevel != S::EXTRA_SPACING_OFF;
  doc["wordSpacingPercent"] = s.wordSpacingPercent;
  doc["wordSpacingMidpoints"] = true;

  // run B: firstLineIndentMode
  emitMechUntil("shortPwrBtn");

  // reader style + text-render mode (migrations + companion flag keys)
  doc["readerStyleMode"] = s.readerStyleMode;
  doc["textRenderMode"] = s.textRenderMode;
  doc["textRenderModeV2"] = true;
  doc["textRenderModeWeightOrder"] = true;
  doc["textRenderModeNormalDark"] = true;
  doc["useFactoryLUT"] = s.useFactoryLUT;  // decode has a side effect (applyFactoryLut)

  // run C: shortPwrBtn, orientation, tiltPageTurn
  emitMechUntil("clockUtcOffsetQ");
  doc["statusBarClock"] = s.statusBarClock;  // decode coerces (x ? 1 : 0)

  // run D: clockUtcOffsetQ
  emitMechUntil("sideButtonLayout");
  doc["clockFormat"] = s.clockFormat;
  doc["clockHasBeenSynced"] = s.clockHasBeenSynced;

  // run E: sideButtonLayout
  emitMechUntil("paragraphAlignment");

  // front-button remap (cross-field validate), font family/size (normalize),
  // line spacing (fallback dependency)
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  doc["fontFamily"] = s.fontFamily;
  doc["fontSize"] = s.fontSize;
  doc["lineSpacing"] = s.lineSpacing;
  doc["lineSpacingPercent"] = s.lineSpacingPercent;

  // run F: paragraphAlignment .. refreshFrequency
  emitMechUntil("smoothText");
  doc["screenMargin"] = s.screenMargin;  // split-margin fallback source

  // run G: smoothText, uniformMargins
  emitMechUntil("hideBatteryPercentage");

  // Defensive clamp: field is 0/1/2 in the UI, but a bad in-memory value
  // would otherwise hit disk and force a reset on load.
  doc["dynamicMargins"] = static_cast<uint8_t>(s.dynamicMargins > 2 ? 0 : s.dynamicMargins);
  doc["screenMarginHorizontal"] = s.screenMarginHorizontal;
  doc["screenMarginTop"] = s.screenMarginTop;
  doc["screenMarginBottom"] = s.screenMarginBottom;
  doc["opdsServerUrl"] = s.opdsServerUrl;
  doc["opdsUsername"] = s.opdsUsername;
  doc["opdsPassword_obf"] = env.obfuscate(s.opdsPassword);

  // run H: hideBatteryPercentage, longPressChapterSkip, hyphenationEnabled
  emitMechUntil("fadingFix");
  doc["uiLanguage"] = s.uiLanguage;  // decode clamps against env.languageCount()

  // run I: fadingFix
  emitMechUntil("debugBorders");
  doc["embeddedStyle"] = s.readerStyleMode == S::READER_STYLE_HYBRID;  // derived

  // run J: debugBorders .. imageDither
  emitMechRest();
}

bool crosspoint::persist::decodeSettings(CrossPointSettings& s, const char* json, const SettingsEnv& env,
                                         bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  using S = CrossPointSettings;

  // Mechanical fields: decode every row of the shared kFields table (encode
  // emits the same rows from the same table, so a table field can never be
  // written-but-not-read). Each row is independent and disjoint from the
  // irregular fields below, so loop order does not matter.
  for (size_t i = 0; i < kFieldCount; ++i) {
    const FieldDesc& f = kFields[i];
    uint8_t v = doc[f.key] | f.deflt;
    if (f.count) {
      v = clampEnum(v, f.count, f.deflt);
    } else if (f.maxRaw && v > f.maxRaw) {
      v = f.deflt;
    }
    s.*(f.member) = v;
  }

  const bool hasGranularStatusBar = !doc["statusBarEnabled"].isNull() && !doc["statusBarShowBattery"].isNull() &&
                                    !doc["statusBarShowPageCounter"].isNull() &&
                                    !doc["statusBarShowBookPercentage"].isNull() &&
                                    !doc["statusBarShowChapterPercentage"].isNull() &&
                                    !doc["statusBarShowBookBar"].isNull() && !doc["statusBarShowChapterBar"].isNull() &&
                                    !doc["statusBarShowChapterTitle"].isNull() && !doc["statusBarTopLine"].isNull() &&
                                    !doc["statusBarTextAlignment"].isNull() && !doc["statusBarProgressStyle"].isNull();
  if (hasGranularStatusBar) {
    s.statusBarEnabled = doc["statusBarEnabled"] | (uint8_t)1;
    s.statusBarShowBattery = doc["statusBarShowBattery"] | (uint8_t)1;
    s.statusBarShowPageCounter = doc["statusBarShowPageCounter"] | (uint8_t)0;
    if (doc["statusBarPageCounterMode"].isNull()) {
      s.statusBarPageCounterMode = (uint8_t)S::STATUS_PAGE_CURRENT_OVER_TOTAL;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarPageCounterMode = S::normalizeStatusBarPageCounterMode(doc["statusBarPageCounterMode"] |
                                                                        (uint8_t)S::STATUS_PAGE_CURRENT_OVER_TOTAL);
    }
    s.statusBarShowBookPercentage = doc["statusBarShowBookPercentage"] | (uint8_t)0;
    s.statusBarShowChapterPercentage = doc["statusBarShowChapterPercentage"] | (uint8_t)0;
    s.statusBarShowBookBar = doc["statusBarShowBookBar"] | (uint8_t)0;
    s.statusBarShowChapterBar = doc["statusBarShowChapterBar"] | (uint8_t)0;
    s.statusBarShowChapterTitle = doc["statusBarShowChapterTitle"] | (uint8_t)1;
    s.statusBarNoTitleTruncation = doc["statusBarNoTitleTruncation"] | (uint8_t)0;
    s.statusBarTopLine = doc["statusBarTopLine"] | (uint8_t)0;
    if (doc["batteryPositionV2"].isNull()) {
      // Migrate old 2-value position (Top/Bottom) to 6-value text position
      const uint8_t old = doc["statusBarBatteryPosition"] | (uint8_t)S::STATUS_AT_BOTTOM;
      s.statusBarBatteryPosition =
          (old == S::STATUS_AT_TOP) ? (uint8_t)S::STATUS_TEXT_TOP_LEFT : (uint8_t)S::STATUS_TEXT_BOTTOM_LEFT;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarBatteryPosition = clampEnum(doc["statusBarBatteryPosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_LEFT,
                                             S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_LEFT);
    }
    if (doc["statusBarProgressTextPosition"].isNull()) {
      s.statusBarProgressTextPosition = (uint8_t)S::STATUS_AT_BOTTOM;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarProgressTextPosition = clampEnum(doc["statusBarProgressTextPosition"] | (uint8_t)S::STATUS_AT_BOTTOM,
                                                  S::STATUS_BAR_ITEM_POSITION_COUNT, S::STATUS_AT_BOTTOM);
    }
    const uint8_t fallbackProgressTextPosition = s.statusBarProgressTextPosition == S::STATUS_AT_TOP
                                                     ? (uint8_t)S::STATUS_TEXT_TOP_CENTER
                                                     : (uint8_t)S::STATUS_TEXT_BOTTOM_CENTER;
    if (doc["statusBarPageCounterPosition"].isNull()) {
      s.statusBarPageCounterPosition = fallbackProgressTextPosition;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarPageCounterPosition =
          clampEnum(doc["statusBarPageCounterPosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_CENTER,
                    S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_CENTER);
    }
    if (doc["statusBarBookPercentagePosition"].isNull()) {
      s.statusBarBookPercentagePosition = fallbackProgressTextPosition;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarBookPercentagePosition =
          clampEnum(doc["statusBarBookPercentagePosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_CENTER,
                    S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_CENTER);
    }
    if (doc["statusBarChapterPercentagePosition"].isNull()) {
      s.statusBarChapterPercentagePosition = fallbackProgressTextPosition;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarChapterPercentagePosition =
          clampEnum(doc["statusBarChapterPercentagePosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_CENTER,
                    S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_CENTER);
    }
    if (doc["statusBarBookBarPosition"].isNull()) {
      s.statusBarBookBarPosition = (uint8_t)S::STATUS_AT_BOTTOM;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarBookBarPosition = clampEnum(doc["statusBarBookBarPosition"] | (uint8_t)S::STATUS_AT_BOTTOM,
                                             S::STATUS_BAR_ITEM_POSITION_COUNT, S::STATUS_AT_BOTTOM);
    }
    if (doc["statusBarChapterBarPosition"].isNull()) {
      s.statusBarChapterBarPosition = (uint8_t)S::STATUS_AT_BOTTOM;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarChapterBarPosition = clampEnum(doc["statusBarChapterBarPosition"] | (uint8_t)S::STATUS_AT_BOTTOM,
                                                S::STATUS_BAR_ITEM_POSITION_COUNT, S::STATUS_AT_BOTTOM);
    }
    if (doc["statusBarTitlePosition"].isNull()) {
      s.statusBarTitlePosition = (uint8_t)S::STATUS_AT_BOTTOM;
      if (needsResave) {
        *needsResave = true;
      }
    } else {
      s.statusBarTitlePosition = clampEnum(doc["statusBarTitlePosition"] | (uint8_t)S::STATUS_AT_BOTTOM,
                                           S::STATUS_BAR_ITEM_POSITION_COUNT, S::STATUS_AT_BOTTOM);
    }
    s.statusBarTextAlignment = clampEnum(doc["statusBarTextAlignment"] | (uint8_t)S::STATUS_TEXT_RIGHT,
                                         S::STATUS_TEXT_ALIGNMENT_COUNT, S::STATUS_TEXT_RIGHT);
    s.statusBarProgressStyle = clampEnum(doc["statusBarProgressStyle"] | (uint8_t)S::STATUS_BAR_THICK,
                                         S::STATUS_BAR_PROGRESS_STYLE_COUNT, S::STATUS_BAR_THICK);
    s.statusBarBarThickness = clampEnum(doc["statusBarBarThickness"] | (uint8_t)S::STATUS_BAR_THICKNESS_NORMAL,
                                        S::STATUS_BAR_BAR_THICKNESS_COUNT, S::STATUS_BAR_THICKNESS_NORMAL);
    // v6.0.0 granular status-bar items (see populateSettingsDoc). Absent from files
    // written before this fix -> fall back to struct defaults; persisted on next save.
    s.statusBarShowPagesLeft = doc["statusBarShowPagesLeft"] | (uint8_t)0;
    s.statusBarPagesLeftPosition = clampEnum(doc["statusBarPagesLeftPosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_RIGHT,
                                             S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_RIGHT);
    s.statusBarTitleContent = clampEnum(doc["statusBarTitleContent"] | (uint8_t)S::STATUS_TITLE_CHAPTER,
                                        S::STATUS_BAR_TITLE_CONTENT_COUNT, S::STATUS_TITLE_CHAPTER);
    s.statusBarShowChapterNumber = doc["statusBarShowChapterNumber"] | (uint8_t)0;
    s.statusBarChapterNumberPosition =
        clampEnum(doc["statusBarChapterNumberPosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_LEFT,
                  S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_LEFT);
    s.statusBarShowQuoteCount = doc["statusBarShowQuoteCount"] | (uint8_t)0;
    s.statusBarQuoteCountPosition = clampEnum(doc["statusBarQuoteCountPosition"] | (uint8_t)S::STATUS_TEXT_BOTTOM_RIGHT,
                                              S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_BOTTOM_RIGHT);
    s.statusBarShowFreeHeap = doc["statusBarShowFreeHeap"] | (uint8_t)0;
    s.statusBarFreeHeapPosition = clampEnum(doc["statusBarFreeHeapPosition"] | (uint8_t)S::STATUS_TEXT_TOP_RIGHT,
                                            S::STATUS_BAR_TEXT_POSITION_COUNT, S::STATUS_TEXT_TOP_RIGHT);
  } else {
    migrateLegacyStatusBarMode(s);
    if (needsResave) *needsResave = true;
  }
  if (!doc["extraParagraphSpacingLevel"].isNull()) {
    s.extraParagraphSpacingLevel = clampEnum(doc["extraParagraphSpacingLevel"] | (uint8_t)S::EXTRA_SPACING_M,
                                             S::EXTRA_PARAGRAPH_SPACING_COUNT, S::EXTRA_SPACING_M);
  } else {
    const uint8_t legacyExtraSpacing = doc["extraParagraphSpacing"] | (uint8_t)1;
    s.extraParagraphSpacingLevel = legacyExtraSpacing ? (uint8_t)S::EXTRA_SPACING_M : (uint8_t)S::EXTRA_SPACING_OFF;
    if (needsResave) *needsResave = true;
  }
  {
    const uint8_t raw = doc["wordSpacingPercent"] | (uint8_t)S::WORD_SPACING_NORMAL;
    if (doc["wordSpacingMidpoints"].isNull()) {
      // Pre-midpoint 5-value numbering -> remap the three wide steps up past the
      // inserted midpoints (TIGHT/NORMAL unchanged).
      s.wordSpacingPercent = S::migrateWordSpacingToMidpoints(raw);
      if (needsResave) *needsResave = true;
    } else if (raw < S::WORD_SPACING_MODE_COUNT) {
      s.wordSpacingPercent = raw;
    } else {
      s.wordSpacingPercent = (uint8_t)S::WORD_SPACING_NORMAL;
      if (needsResave) *needsResave = true;
    }
  }
  if (doc["readerStyleMode"].isNull()) {
    s.readerStyleMode =
        doc["embeddedStyle"].isNull()
            ? (uint8_t)S::READER_STYLE_USER
            : ((doc["embeddedStyle"] | (uint8_t)0) ? (uint8_t)S::READER_STYLE_HYBRID : (uint8_t)S::READER_STYLE_USER);
    if (needsResave) {
      *needsResave = true;
    }
  } else {
    s.readerStyleMode = clampEnum(doc["readerStyleMode"] | (uint8_t)S::READER_STYLE_USER, S::READER_STYLE_MODE_COUNT,
                                  S::READER_STYLE_USER);
  }
  if (doc["textRenderMode"].isNull()) {
    s.textRenderMode = (uint8_t)S::TEXT_RENDER_NORMAL;
    if (needsResave) {
      *needsResave = true;
    }
  } else {
    // Render mode is now a 2-way user setting (Normal=0, Dark=1). Older files are
    // migrated by flag: textRenderModeNormalDark -> already current; otherwise
    // resolve to the weight-order STYLE palette and collapse (only Dark stays Dark):
    //   no textRenderModeV2  -> pre-v2 (only Crisp/Dark): non-zero -> Dark.
    //   textRenderModeV2 only -> v2 numbering (Crisp=0,Dark=1,Bionic=2,Thin=3).
    //   textRenderModeWeightOrder -> weight-order style (0..4).
    const uint8_t raw = doc["textRenderMode"] | (uint8_t)S::TEXT_RENDER_NORMAL;
    if (!doc["textRenderModeNormalDark"].isNull()) {
      s.textRenderMode = raw < S::TEXT_RENDER_MODE_COUNT ? raw : (uint8_t)S::TEXT_RENDER_NORMAL;
    } else {
      uint8_t style;
      if (doc["textRenderModeV2"].isNull()) {
        style = raw >= 1 ? S::kRenderStyleDark : S::kRenderStyleCrisp;
      } else if (doc["textRenderModeWeightOrder"].isNull()) {
        style = S::migrateTextRenderModeToWeightOrder(raw);
      } else {
        style = raw;
      }
      s.textRenderMode = S::collapseRenderStyleToMode(style);
      if (needsResave) {
        *needsResave = true;
      }
    }
  }
  s.textAntiAliasing = 0;
  s.useFactoryLUT = (doc["useFactoryLUT"] | 0) ? 1 : 0;
  env.applyFactoryLut(s.useFactoryLUT != 0);
  s.statusBarClock = (doc["statusBarClock"] | 0) ? 1 : 0;
  s.clockFormat = (doc["clockFormat"] | 0) ? 1 : 0;
  s.clockHasBeenSynced = (doc["clockHasBeenSynced"] | 0) ? 1 : 0;
  s.frontButtonBack =
      clampEnum(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clampEnum(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM,
                                   S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clampEnum(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight = clampEnum(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT,
                                 S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);
  // fontFamily/fontSize: cross-field normalize (size depends on family).
  const bool ffPresent = !doc["fontFamily"].isNull();
  const int ffRaw = ffPresent ? (int)(doc["fontFamily"] | (uint8_t)S::CHAREINK) : -1;
  s.fontFamily = clampEnum(doc["fontFamily"] | (uint8_t)S::CHAREINK, S::FONT_FAMILY_COUNT, S::CHAREINK);
  const uint8_t ffClamped = s.fontFamily;
  s.fontFamily = S::normalizeFontFamily(s.fontFamily);
  LOG_DIAG("CPS", "loadSettings: ff_present=%d ff_raw=%d ff_clamped=%u ff_normalized=%u", ffPresent ? 1 : 0, ffRaw,
           (unsigned)ffClamped, (unsigned)s.fontFamily);
  s.fontSize = clampEnum(doc["fontSize"] | (uint8_t)S::SIZE_16, S::FONT_SIZE_COUNT, S::SIZE_16);
  s.fontSize = S::normalizeFontSizeForFamily(s.fontFamily, s.fontSize);
  s.lineSpacing = clampEnum(doc["lineSpacing"] | (uint8_t)S::NORMAL, S::LINE_COMPRESSION_COUNT, S::NORMAL);
  if (!doc["lineSpacingPercent"].isNull()) {
    const uint8_t parsed = doc["lineSpacingPercent"] | (uint8_t)110;
    if (parsed < 35) {
      s.lineSpacingPercent = 35;
    } else if (parsed > 150) {
      s.lineSpacingPercent = 150;
    } else {
      s.lineSpacingPercent = parsed;
    }
  } else {
    switch (s.lineSpacing) {
      case S::TIGHT:
        s.lineSpacingPercent = 95;
        break;
      case S::WIDE:
        s.lineSpacingPercent = 125;
        break;
      case S::NORMAL:
      default:
        s.lineSpacingPercent = 110;
        break;
    }
    if (needsResave) {
      *needsResave = true;
    }
  }
  s.screenMargin = doc["screenMargin"] | (uint8_t)5;  // split-margin fallback source
  s.dynamicMargins = doc["dynamicMargins"] | (uint8_t)0;
  if (s.dynamicMargins > 2) s.dynamicMargins = 0;
  const bool hasSplitMargins = !doc["screenMarginHorizontal"].isNull() && !doc["screenMarginTop"].isNull() &&
                               !doc["screenMarginBottom"].isNull();
  if (hasSplitMargins) {
    s.screenMarginHorizontal = doc["screenMarginHorizontal"] | s.screenMargin;
    s.screenMarginTop = doc["screenMarginTop"] | s.screenMargin;
    s.screenMarginBottom = doc["screenMarginBottom"] | s.screenMargin;
  } else {
    s.screenMarginHorizontal = s.screenMargin;
    s.screenMarginTop = s.screenMargin;
    s.screenMarginBottom = s.screenMargin;
    if (needsResave) *needsResave = true;
  }
  // uiLanguage: clamp against the injected language count (env dependency).
  s.uiLanguage = clampEnum(doc["uiLanguage"] | (uint8_t)0, env.languageCount(), 0);
  // embeddedStyle: derived from readerStyleMode (legacy compat field).
  s.embeddedStyle = s.readerStyleMode == S::READER_STYLE_HYBRID ? (uint8_t)1 : (uint8_t)0;

  const char* url = doc["opdsServerUrl"] | "";
  copyCStr(s.opdsServerUrl, url);

  const char* user = doc["opdsUsername"] | "";
  copyCStr(s.opdsUsername, user);

  bool passOk = false;
  std::string pass = env.deobfuscate(doc["opdsPassword_obf"] | "", &passOk);
  if (!passOk || pass.empty()) {
    pass = doc["opdsPassword"] | "";
    if (!pass.empty() && needsResave) *needsResave = true;
  }
  copyCStr(s.opdsPassword, pass.c_str());
  LOG_DIAG("CPS", "loadSettings: ff=%u fs=%u lsp=%u", (unsigned)s.fontFamily, (unsigned)s.fontSize,
           (unsigned)s.lineSpacingPercent);
  return true;
}
