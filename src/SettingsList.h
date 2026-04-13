#pragma once

#include <I18n.h>

#include <vector>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "activities/settings/SettingsActivity.h"

// Shared settings list used by both the device settings UI and the web settings API.
// Each entry has a key (for JSON API) and category (for grouping).
// ACTION-type entries and entries without a key are device-only.
inline std::vector<SettingInfo> getSettingsList() {
  return {
      // --- Display ---
      SettingInfo::Enum(StrId::STR_SLEEP_SCREEN, &CrossPointSettings::sleepScreen,
                        {StrId::STR_DARK, StrId::STR_LIGHT, StrId::STR_CUSTOM, StrId::STR_COVER, StrId::STR_NONE_OPT,
                         StrId::STR_COVER_CUSTOM},
                        "sleepScreen", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_MODE, &CrossPointSettings::sleepScreenCoverMode,
                        {StrId::STR_FIT, StrId::STR_CROP}, "sleepScreenCoverMode", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_SLEEP_COVER_FILTER, &CrossPointSettings::sleepScreenCoverFilter,
                        {StrId::STR_NONE_OPT, StrId::STR_FILTER_CONTRAST, StrId::STR_INVERTED},
                        "sleepScreenCoverFilter", StrId::STR_CAT_DISPLAY),
      SettingInfo::Toggle(StrId::STR_SHOW_SLEEP_IMAGE_FILENAME, &CrossPointSettings::showSleepImageFilename,
                          "showSleepImageFilename", StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_HIDE_BATTERY, &CrossPointSettings::hideBatteryPercentage,
                        {StrId::STR_NEVER, StrId::STR_IN_READER, StrId::STR_ALWAYS}, "hideBatteryPercentage",
                        StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(
          StrId::STR_REFRESH_FREQ, &CrossPointSettings::refreshFrequency,
          {StrId::STR_PAGES_1, StrId::STR_PAGES_5, StrId::STR_PAGES_10, StrId::STR_PAGES_15, StrId::STR_PAGES_30},
          "refreshFrequency", StrId::STR_CAT_DISPLAY),
      SettingInfo::Toggle(StrId::STR_SUNLIGHT_FADING_FIX, &CrossPointSettings::fadingFix, "fadingFix",
                          StrId::STR_CAT_DISPLAY),
      SettingInfo::Enum(StrId::STR_HOME_LAYOUT, &CrossPointSettings::homeLayout,
                        {StrId::STR_HOME_LAYOUT_CLASSIC, StrId::STR_HOME_LAYOUT_SINGLE_COVER},
                        "homeLayout", StrId::STR_CAT_DISPLAY),

      // --- Reader ---
      SettingInfo::Enum(StrId::STR_FONT_FAMILY, &CrossPointSettings::fontFamily,
                        {StrId::STR_CHAREINK, StrId::STR_BOOKERLY, StrId::STR_VOLLKORN}, "fontFamily",
                        StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_FONT_SIZE, &CrossPointSettings::fontSize,
                        {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE}, "fontSize",
                        StrId::STR_CAT_READER),
      SettingInfo::Value(StrId::STR_LINE_SPACING, &CrossPointSettings::lineSpacingPercent, {65, 150, 5},
                         "lineSpacingPercent", StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_DYNAMIC_MARGINS, &CrossPointSettings::dynamicMargins,
                          "dynamicMargins", StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_UNIFORM_MARGINS, &CrossPointSettings::uniformMargins,
                          "uniformMargins", StrId::STR_CAT_READER),
      // Uniform margin entry (shown when uniformMargins == 1)
      SettingInfo::Value(StrId::STR_SCREEN_MARGIN, &CrossPointSettings::screenMarginHorizontal, {0, 55, 5},
                         "screenMarginHorizontal", StrId::STR_CAT_READER),
      // Separate margin entries (shown when uniformMargins == 0)
      SettingInfo::Value(StrId::STR_SCREEN_MARGIN_HORIZONTAL, &CrossPointSettings::screenMarginHorizontal, {0, 55, 5},
                         "screenMarginHorizontal", StrId::STR_CAT_READER),
      SettingInfo::Value(StrId::STR_SCREEN_MARGIN_TOP, &CrossPointSettings::screenMarginTop, {0, 55, 5},
                         "screenMarginTop", StrId::STR_CAT_READER),
      SettingInfo::Value(StrId::STR_SCREEN_MARGIN_BOTTOM, &CrossPointSettings::screenMarginBottom, {0, 55, 5},
                         "screenMarginBottom", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_PARA_ALIGNMENT, &CrossPointSettings::paragraphAlignment,
                        {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                         StrId::STR_BOOK_S_STYLE},
                        "paragraphAlignment", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_FIRST_LINE_INDENT, &CrossPointSettings::firstLineIndentMode,
                        {StrId::STR_BOOK_STYLE_OPT, StrId::STR_NONE_OPT, StrId::STR_INDENT_SMALL,
                         StrId::STR_INDENT_MEDIUM, StrId::STR_INDENT_LARGE},
                        "firstLineIndentMode", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_READER_STYLE_MODE, &CrossPointSettings::readerStyleMode,
                        {StrId::STR_READER_STYLE_USER, StrId::STR_READER_STYLE_HYBRID},
                        "readerStyleMode", StrId::STR_CAT_READER),
      SettingInfo::Toggle(StrId::STR_DEBUG_BORDERS, &CrossPointSettings::debugBorders, "debugBorders",
                          StrId::STR_CAT_READER),
      // Highlight mode removed — word-based selection is the only mode
      SettingInfo::Enum(StrId::STR_ORIENTATION, &CrossPointSettings::orientation,
                        {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW},
                        "orientation", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_WORD_SPACING, &CrossPointSettings::wordSpacingPercent,
                        {StrId::STR_WSPACING_M30, StrId::STR_WSPACING_0, StrId::STR_WSPACING_P80, StrId::STR_WSPACING_P150, StrId::STR_WSPACING_P240},
                        "wordSpacingPercent", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_EXTRA_SPACING, &CrossPointSettings::extraParagraphSpacingLevel,
                        {StrId::STR_NONE_OPT, StrId::STR_PARA_SPACING_17, StrId::STR_PARA_SPACING_25, StrId::STR_PARA_SPACING_33},
                        "extraParagraphSpacingLevel", StrId::STR_CAT_READER),
      SettingInfo::Enum(StrId::STR_TEXT_RENDER_MODE, &CrossPointSettings::textRenderMode,
                        {StrId::STR_RENDER_CRISP, StrId::STR_RENDER_DARK},
                        "textRenderMode", StrId::STR_CAT_READER),

      // Status bar customization
      SettingInfo::Toggle(StrId::STR_STATUS_BAR, &CrossPointSettings::statusBarEnabled, "statusBarEnabled",
                          StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_BATTERY, &CrossPointSettings::statusBarShowBattery, "statusBarShowBattery",
                          StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_BATTERY_POSITION, &CrossPointSettings::statusBarBatteryPosition,
                        {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER,
                         StrId::STR_STATUS_POS_TOP_RIGHT, StrId::STR_STATUS_POS_BOTTOM_LEFT,
                         StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT},
                        "statusBarBatteryPosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_PAGE_COUNTER, &CrossPointSettings::statusBarShowPageCounter,
                          "statusBarShowPageCounter", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_PAGE_COUNTER_MODE, &CrossPointSettings::statusBarPageCounterMode,
                        {StrId::STR_STATUS_PAGE_MODE_CURRENT_TOTAL, StrId::STR_STATUS_PAGE_MODE_LEFT_CHAPTER},
                        "statusBarPageCounterMode", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_PAGE_COUNTER_POSITION, &CrossPointSettings::statusBarPageCounterPosition,
                        {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER,
                         StrId::STR_STATUS_POS_TOP_RIGHT, StrId::STR_STATUS_POS_BOTTOM_LEFT,
                         StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT},
                        "statusBarPageCounterPosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_BOOK_PERCENT, &CrossPointSettings::statusBarShowBookPercentage,
                          "statusBarShowBookPercentage", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_BOOK_PERCENT_POSITION, &CrossPointSettings::statusBarBookPercentagePosition,
                        {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER,
                         StrId::STR_STATUS_POS_TOP_RIGHT, StrId::STR_STATUS_POS_BOTTOM_LEFT,
                         StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT},
                        "statusBarBookPercentagePosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_PERCENT, &CrossPointSettings::statusBarShowChapterPercentage,
                          "statusBarShowChapterPercentage", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_CHAPTER_PERCENT_POSITION, &CrossPointSettings::statusBarChapterPercentagePosition,
                        {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER,
                         StrId::STR_STATUS_POS_TOP_RIGHT, StrId::STR_STATUS_POS_BOTTOM_LEFT,
                         StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT},
                        "statusBarChapterPercentagePosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_BOOK_BAR, &CrossPointSettings::statusBarShowBookBar, "statusBarShowBookBar",
                          StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_BOOK_BAR_POSITION, &CrossPointSettings::statusBarBookBarPosition,
                        {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM},
                        "statusBarBookBarPosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_BAR, &CrossPointSettings::statusBarShowChapterBar,
                          "statusBarShowChapterBar", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_CHAPTER_BAR_POSITION, &CrossPointSettings::statusBarChapterBarPosition,
                        {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM},
                        "statusBarChapterBarPosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_CHAPTER_TITLE, &CrossPointSettings::statusBarShowChapterTitle,
                          "statusBarShowChapterTitle", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_CHAPTER_TITLE_POSITION, &CrossPointSettings::statusBarTitlePosition,
                        {StrId::STR_STATUS_POSITION_TOP, StrId::STR_STATUS_POSITION_BOTTOM},
                        "statusBarTitlePosition", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_NO_TITLE_TRUNCATION, &CrossPointSettings::statusBarNoTitleTruncation,
                          "statusBarNoTitleTruncation", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_FONT_SIZE, &CrossPointSettings::statusBarFontSize,
                        {StrId::STR_STATUS_FONT_MIN, StrId::STR_STATUS_FONT_MAX}, "statusBarFontSize", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_BAR_THICKNESS, &CrossPointSettings::statusBarBarThickness,
                        {StrId::STR_STATUS_BAR_THICKNESS_NORMAL, StrId::STR_STATUS_BAR_THICKNESS_DOUBLE}, "statusBarBarThickness", StrId::STR_STATUS_BAR),
      SettingInfo::Toggle(StrId::STR_STATUS_BOOK_PAGE_COUNTER, &CrossPointSettings::statusBarShowBookPageCounter,
                          "statusBarShowBookPageCounter", StrId::STR_STATUS_BAR),
      SettingInfo::Enum(StrId::STR_STATUS_BOOK_PAGE_COUNTER_POSITION, &CrossPointSettings::statusBarBookPageCounterPosition,
                        {StrId::STR_STATUS_POS_TOP_LEFT, StrId::STR_STATUS_POS_TOP_CENTER,
                         StrId::STR_STATUS_POS_TOP_RIGHT, StrId::STR_STATUS_POS_BOTTOM_LEFT,
                         StrId::STR_STATUS_POS_BOTTOM_CENTER, StrId::STR_STATUS_POS_BOTTOM_RIGHT},
                        "statusBarBookPageCounterPosition", StrId::STR_STATUS_BAR),

      // --- Controls ---
      SettingInfo::Enum(StrId::STR_SIDE_BTN_LAYOUT, &CrossPointSettings::sideButtonLayout,
                        {StrId::STR_PREV_NEXT, StrId::STR_NEXT_PREV}, "sideButtonLayout", StrId::STR_CAT_CONTROLS),
      SettingInfo::Toggle(StrId::STR_LONG_PRESS_SKIP, &CrossPointSettings::longPressChapterSkip, "longPressChapterSkip",
                          StrId::STR_CAT_CONTROLS),
      SettingInfo::Enum(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn,
                        {StrId::STR_IGNORE, StrId::STR_SLEEP, StrId::STR_PAGE_TURN, StrId::STR_FORCE_REFRESH},
                        "shortPwrBtn", StrId::STR_CAT_CONTROLS),

      // --- System ---
      SettingInfo::Enum(StrId::STR_TIME_TO_SLEEP, &CrossPointSettings::sleepTimeout,
                        {StrId::STR_MIN_1, StrId::STR_MIN_5, StrId::STR_MIN_10, StrId::STR_MIN_15, StrId::STR_MIN_30},
                        "sleepTimeout", StrId::STR_CAT_SYSTEM),
      SettingInfo::Toggle(StrId::STR_SHOW_HIDDEN_FILES, &CrossPointSettings::showHiddenFiles, "showHiddenFiles",
                          StrId::STR_CAT_SYSTEM),
      SettingInfo::Toggle(StrId::STR_RANDOM_BOOK_ON_BOOT, &CrossPointSettings::randomBookOnBoot, "randomBookOnBoot",
                          StrId::STR_CAT_SYSTEM),

      // --- KOReader Sync (web-only, uses KOReaderCredentialStore) ---
      SettingInfo::DynamicString(
          StrId::STR_KOREADER_USERNAME, [] { return KOREADER_STORE.getUsername(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(v, KOREADER_STORE.getPassword());
            KOREADER_STORE.saveToFile();
          },
          "koUsername", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::STR_KOREADER_PASSWORD, [] { return KOREADER_STORE.getPassword(); },
          [](const std::string& v) {
            KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), v);
            KOREADER_STORE.saveToFile();
          },
          "koPassword", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicString(
          StrId::STR_SYNC_SERVER_URL, [] { return KOREADER_STORE.getServerUrl(); },
          [](const std::string& v) {
            KOREADER_STORE.setServerUrl(v);
            KOREADER_STORE.saveToFile();
          },
          "koServerUrl", StrId::STR_KOREADER_SYNC),
      SettingInfo::DynamicEnum(
          StrId::STR_DOCUMENT_MATCHING, {StrId::STR_FILENAME, StrId::STR_BINARY},
          [] { return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod()); },
          [](uint8_t v) {
            KOREADER_STORE.setMatchMethod(static_cast<DocumentMatchMethod>(v));
            KOREADER_STORE.saveToFile();
          },
          "koMatchMethod", StrId::STR_KOREADER_SYNC),

      // --- OPDS Browser (web-only, uses CrossPointSettings char arrays) ---
      SettingInfo::String(StrId::STR_OPDS_SERVER_URL, SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl),
                          "opdsServerUrl", StrId::STR_OPDS_BROWSER),
      SettingInfo::String(StrId::STR_USERNAME, SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), "opdsUsername",
                          StrId::STR_OPDS_BROWSER),
      SettingInfo::String(StrId::STR_PASSWORD, SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), "opdsPassword",
                          StrId::STR_OPDS_BROWSER),
  };
}

