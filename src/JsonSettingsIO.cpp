#include "JsonSettingsIO.h"

#include <ArduinoJson.h>
#include <BitmapHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "ReadingThemeStore.h"
#include "RecentBooksStore.h"
#include "WifiCredentialStore.h"
#include "util/StringUtils.h"

namespace {
// migrateLegacyStatusBarMode is now declared in CrossPointSettings.h
// and defined in CrossPointSettings.cpp (single source of truth).

// Clamp a persisted enum-valued byte: if val is out of range, fall back to def.
// Used by both readReadingThemeObject() and loadSettings() to validate enums
// read from the settings JSON.
inline uint8_t clampEnum(uint8_t val, uint8_t maxVal, uint8_t def) { return val < maxVal ? val : def; }

void writeReadingThemeObject(JsonObject obj, const ReadingTheme& theme) {
  obj["name"] = theme.name;
  obj["fontFamily"] = theme.fontFamily;
  obj["fontSize"] = theme.fontSize;
  obj["customFontName"] = theme.customFontName;
  obj["customFontSizePt"] = theme.customFontSizePt;
  obj["lineSpacingPercent"] = theme.lineSpacingPercent;
  obj["uniformMargins"] = theme.uniformMargins;
  obj["dynamicMargins"] = theme.dynamicMargins;
  obj["screenMarginHorizontal"] = theme.screenMarginHorizontal;
  obj["screenMarginTop"] = theme.screenMarginTop;
  obj["screenMarginBottom"] = theme.screenMarginBottom;
  obj["paragraphAlignment"] = theme.paragraphAlignment;
  obj["extraParagraphSpacingLevel"] = theme.extraParagraphSpacingLevel;
  obj["wordSpacingPercent"] = theme.wordSpacingPercent;
  obj["firstLineIndentMode"] = theme.firstLineIndentMode;
  obj["readerStyleMode"] = theme.readerStyleMode;
  obj["textRenderMode"] = theme.textRenderMode;
  obj["textRenderModeV2"] = true;
  obj["orientation"] = theme.orientation;
  obj["embeddedStyle"] = theme.readerStyleMode == CrossPointSettings::READER_STYLE_HYBRID;
  obj["hyphenationEnabled"] = theme.hyphenationEnabled;
  obj["statusBarEnabled"] = theme.statusBarEnabled;
  obj["statusBarShowBattery"] = theme.statusBarShowBattery;
  obj["statusBarShowPageCounter"] = theme.statusBarShowPageCounter;
  obj["statusBarPageCounterMode"] = theme.statusBarPageCounterMode;
  obj["statusBarShowBookPercentage"] = theme.statusBarShowBookPercentage;
  obj["statusBarShowChapterPercentage"] = theme.statusBarShowChapterPercentage;
  obj["statusBarShowBookBar"] = theme.statusBarShowBookBar;
  obj["statusBarShowChapterBar"] = theme.statusBarShowChapterBar;
  obj["statusBarShowChapterTitle"] = theme.statusBarShowChapterTitle;
  obj["statusBarNoTitleTruncation"] = theme.statusBarNoTitleTruncation;
  obj["statusBarBatteryPosition"] = theme.statusBarBatteryPosition;
  obj["batteryPositionV2"] = true;
  obj["statusBarProgressTextPosition"] = theme.statusBarProgressTextPosition;
  obj["statusBarPageCounterPosition"] = theme.statusBarPageCounterPosition;
  obj["statusBarBookPercentagePosition"] = theme.statusBarBookPercentagePosition;
  obj["statusBarChapterPercentagePosition"] = theme.statusBarChapterPercentagePosition;
  obj["statusBarBookBarPosition"] = theme.statusBarBookBarPosition;
  obj["statusBarChapterBarPosition"] = theme.statusBarChapterBarPosition;
  obj["statusBarTitlePosition"] = theme.statusBarTitlePosition;
  obj["statusBarTextAlignment"] = theme.statusBarTextAlignment;
  obj["statusBarProgressStyle"] = theme.statusBarProgressStyle;
  obj["statusBarFontSize"] = theme.statusBarFontSize;
  obj["statusBarBarThickness"] = theme.statusBarBarThickness;
  obj["statusBarShowBookPageCounter"] = theme.statusBarShowBookPageCounter;
  obj["statusBarBookPageCounterPosition"] = theme.statusBarBookPageCounterPosition;
}

void readReadingThemeObject(JsonObject obj, ReadingTheme& theme) {
  theme.name = ReadingThemeStore::sanitizeName(obj["name"] | "Theme");
  theme.fontFamily = clampEnum(obj["fontFamily"] | (uint8_t)CrossPointSettings::CHAREINK,
                               CrossPointSettings::FONT_FAMILY_COUNT, CrossPointSettings::CHAREINK);
  theme.fontSize = clampEnum(obj["fontSize"] | (uint8_t)CrossPointSettings::SIZE_16,
                             CrossPointSettings::FONT_SIZE_COUNT, CrossPointSettings::SIZE_16);
  theme.customFontName = std::string(obj["customFontName"] | "");
  theme.customFontSizePt = obj["customFontSizePt"] | (uint8_t)0;
  theme.lineSpacingPercent = obj["lineSpacingPercent"] | (uint8_t)110;
  theme.uniformMargins = obj["uniformMargins"] | (uint8_t)0;
  if (theme.uniformMargins > 1) theme.uniformMargins = 0;
  theme.dynamicMargins = obj["dynamicMargins"] | (uint8_t)0;
  if (theme.dynamicMargins > 2) theme.dynamicMargins = 0;
  theme.screenMarginHorizontal = obj["screenMarginHorizontal"] | (uint8_t)20;
  theme.screenMarginTop = obj["screenMarginTop"] | (uint8_t)20;
  theme.screenMarginBottom = obj["screenMarginBottom"] | (uint8_t)20;
  theme.paragraphAlignment = clampEnum(obj["paragraphAlignment"] | (uint8_t)CrossPointSettings::JUSTIFIED,
                                       CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT, CrossPointSettings::JUSTIFIED);
  theme.extraParagraphSpacingLevel =
      clampEnum(obj["extraParagraphSpacingLevel"] | (uint8_t)CrossPointSettings::EXTRA_SPACING_M,
                CrossPointSettings::EXTRA_PARAGRAPH_SPACING_COUNT, CrossPointSettings::EXTRA_SPACING_M);
  theme.wordSpacingPercent =
      clampEnum(obj["wordSpacingPercent"] | (uint8_t)CrossPointSettings::WORD_SPACING_NORMAL,
                CrossPointSettings::WORD_SPACING_MODE_COUNT, CrossPointSettings::WORD_SPACING_NORMAL);
  theme.firstLineIndentMode =
      clampEnum(obj["firstLineIndentMode"] | (uint8_t)CrossPointSettings::INDENT_BOOK,
                CrossPointSettings::FIRST_LINE_INDENT_MODE_COUNT, CrossPointSettings::INDENT_BOOK);
  {
    const uint8_t raw = obj["readerStyleMode"] |
                        (obj["embeddedStyle"].isNull()
                             ? (uint8_t)CrossPointSettings::READER_STYLE_USER
                             : ((obj["embeddedStyle"] | (uint8_t)0) ? (uint8_t)CrossPointSettings::READER_STYLE_HYBRID
                                                                    : (uint8_t)CrossPointSettings::READER_STYLE_USER));
    theme.readerStyleMode =
        (raw < CrossPointSettings::READER_STYLE_MODE_COUNT) ? raw : (uint8_t)CrossPointSettings::READER_STYLE_USER;
  }
  {
    const uint8_t raw = obj["textRenderMode"] | (uint8_t)CrossPointSettings::TEXT_RENDER_CRISP;
    if (obj["textRenderModeV2"].isNull()) {
      // Old v2 format: 0=Crisp, 1=Dark, 2=Dark(old), 3=ExtraDark(old) → map to new enum
      if (raw >= 1) {
        theme.textRenderMode = (uint8_t)CrossPointSettings::TEXT_RENDER_DARK;
      } else {
        theme.textRenderMode = (uint8_t)CrossPointSettings::TEXT_RENDER_CRISP;
      }
    } else if (raw >= CrossPointSettings::TEXT_RENDER_MODE_COUNT) {
      theme.textRenderMode = (uint8_t)CrossPointSettings::TEXT_RENDER_CRISP;
    } else {
      theme.textRenderMode = raw;
    }
  }
  theme.hyphenationEnabled = obj["hyphenationEnabled"] | (uint8_t)0;
  theme.orientation = clampEnum(obj["orientation"] | (uint8_t)CrossPointSettings::PORTRAIT,
                                CrossPointSettings::ORIENTATION_COUNT, CrossPointSettings::PORTRAIT);
  theme.statusBarEnabled = obj["statusBarEnabled"] | (uint8_t)1;
  theme.statusBarShowBattery = obj["statusBarShowBattery"] | (uint8_t)1;
  theme.statusBarShowPageCounter = obj["statusBarShowPageCounter"] | (uint8_t)0;
  theme.statusBarPageCounterMode = CrossPointSettings::normalizeStatusBarPageCounterMode(
      obj["statusBarPageCounterMode"] | (uint8_t)CrossPointSettings::STATUS_PAGE_CURRENT_OVER_TOTAL);
  theme.statusBarShowBookPercentage = obj["statusBarShowBookPercentage"] | (uint8_t)0;
  theme.statusBarShowChapterPercentage = obj["statusBarShowChapterPercentage"] | (uint8_t)0;
  theme.statusBarShowBookBar = obj["statusBarShowBookBar"] | (uint8_t)0;
  theme.statusBarShowChapterBar = obj["statusBarShowChapterBar"] | (uint8_t)0;
  theme.statusBarShowChapterTitle = obj["statusBarShowChapterTitle"] | (uint8_t)1;
  theme.statusBarNoTitleTruncation = obj["statusBarNoTitleTruncation"] | (uint8_t)0;
  if (obj["batteryPositionV2"].isNull()) {
    // Migrate old 2-value position (Top/Bottom) to 6-value text position
    const uint8_t old = obj["statusBarBatteryPosition"] | (uint8_t)CrossPointSettings::STATUS_AT_BOTTOM;
    theme.statusBarBatteryPosition = (old == CrossPointSettings::STATUS_AT_TOP)
                                         ? (uint8_t)CrossPointSettings::STATUS_TEXT_TOP_LEFT
                                         : (uint8_t)CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT;
  } else {
    theme.statusBarBatteryPosition =
        obj["statusBarBatteryPosition"] | (uint8_t)CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT;
  }
  theme.statusBarProgressTextPosition =
      obj["statusBarProgressTextPosition"] | (uint8_t)CrossPointSettings::STATUS_AT_BOTTOM;
  const uint8_t fallbackProgressTextPosition = theme.statusBarProgressTextPosition == CrossPointSettings::STATUS_AT_TOP
                                                   ? (uint8_t)CrossPointSettings::STATUS_TEXT_TOP_CENTER
                                                   : (uint8_t)CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  theme.statusBarPageCounterPosition = obj["statusBarPageCounterPosition"] | fallbackProgressTextPosition;
  theme.statusBarBookPercentagePosition = obj["statusBarBookPercentagePosition"] | fallbackProgressTextPosition;
  theme.statusBarChapterPercentagePosition = obj["statusBarChapterPercentagePosition"] | fallbackProgressTextPosition;
  theme.statusBarBookBarPosition = obj["statusBarBookBarPosition"] | (uint8_t)CrossPointSettings::STATUS_AT_BOTTOM;
  theme.statusBarChapterBarPosition =
      obj["statusBarChapterBarPosition"] | (uint8_t)CrossPointSettings::STATUS_AT_BOTTOM;
  theme.statusBarTitlePosition = obj["statusBarTitlePosition"] | (uint8_t)CrossPointSettings::STATUS_AT_BOTTOM;
  theme.statusBarTextAlignment = obj["statusBarTextAlignment"] | (uint8_t)CrossPointSettings::STATUS_TEXT_RIGHT;
  theme.statusBarProgressStyle = obj["statusBarProgressStyle"] | (uint8_t)CrossPointSettings::STATUS_BAR_THICK;
  theme.statusBarFontSize = obj["statusBarFontSize"] | (uint8_t)CrossPointSettings::STATUS_FONT_SMALL;
  theme.statusBarBarThickness = obj["statusBarBarThickness"] | (uint8_t)CrossPointSettings::STATUS_BAR_THICKNESS_NORMAL;
  theme.statusBarShowBookPageCounter = obj["statusBarShowBookPageCounter"] | (uint8_t)0;
  theme.statusBarBookPageCounterPosition =
      obj["statusBarBookPageCounterPosition"] | (uint8_t)CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  theme = ReadingThemeStore::normalizeTheme(theme);
}
}  // namespace

// ---- Atomic write & read helpers ----
// FAT32 does not support atomic rename over existing files.
// To prevent dataloss on crash, we rotate backups:
// 1. Write to .tmp
// 2. Erase old .bak
// 3. Rename current to .bak
// 4. Rename .tmp to current
static bool safeWriteFile(const char* path, const String& json) {
  // Path + ".corrupt" (longest suffix) must fit in 128-char buffers used below.
  if (!path || strlen(path) > 119) {
    LOG_ERR("JSN", "safeWriteFile: path null or too long");
    return false;
  }

  auto ensureParentDirectory = [](const char* targetPath) -> bool {
    const char* slash = strrchr(targetPath, '/');
    if (!slash || slash == targetPath) {
      return true;
    }

    char parentPath[128];
    const size_t parentLen = static_cast<size_t>(slash - targetPath);
    if (parentLen >= sizeof(parentPath)) {
      LOG_ERR("JSN", "safeWriteFile: parent path too long for %s", targetPath);
      return false;
    }

    memcpy(parentPath, targetPath, parentLen);
    parentPath[parentLen] = '\0';

    if (Storage.exists(parentPath)) {
      FsFile entry = Storage.open(parentPath, O_RDONLY);
      if (entry) {
        const bool isDirectory = entry.isDirectory();
        entry.close();
        if (isDirectory) {
          return true;
        }
      }

      char quarantinePath[160];
      snprintf(quarantinePath, sizeof(quarantinePath), "%s.corrupt", parentPath);
      if (Storage.exists(quarantinePath)) {
        if (!Storage.remove(quarantinePath)) {
          Storage.removeDir(quarantinePath);
        }
      }

      if (!Storage.rename(parentPath, quarantinePath)) {
        if (!Storage.remove(parentPath)) {
          LOG_ERR("JSN", "safeWriteFile: failed to quarantine invalid parent %s", parentPath);
          return false;
        }
      } else {
        LOG_ERR("JSN", "safeWriteFile: quarantined invalid parent %s", parentPath);
      }
    }

    if (!Storage.mkdir(parentPath)) {
      FsFile dir = Storage.open(parentPath, O_RDONLY);
      if (!dir || !dir.isDirectory()) {
        if (dir) {
          dir.close();
        }
        LOG_ERR("JSN", "safeWriteFile: failed to create parent %s", parentPath);
        return false;
      }
      dir.close();
    }

    return true;
  };

  if (!ensureParentDirectory(path)) {
    return false;
  }

  char tmpPath[128];
  char bakPath[128];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);

  // 1. Write to temp file.
  // Explicitly remove any stale .tmp from a previous interrupted write.
  // If the entry is so corrupted it can't even be deleted, fall back to an
  // alternate temp name so saves keep working.
  const char* activeTmp = tmpPath;
  char altTmpPath[128];
  if (Storage.exists(tmpPath)) {
    if (!Storage.remove(tmpPath)) {
      LOG_ERR("JSN",
              "safeWriteFile: stale tmp %s stuck (cannot remove); "
              "falling back to alternate tmp name",
              tmpPath);
      snprintf(altTmpPath, sizeof(altTmpPath), "%s.tmp2", path);
      activeTmp = altTmpPath;
    }
  }
  if (!Storage.writeFile(activeTmp, json)) {
    LOG_ERR("JSN", "safeWriteFile: failed to write tmp %s", activeTmp);
    return false;
  }
  LOG_DIAG("JSN", "safeWriteFile step1 wrote tmp=%s bytes=%u", activeTmp, (unsigned)json.length());

  // 2. Remove stale backup
  if (Storage.exists(bakPath)) {
    if (!Storage.remove(bakPath)) {
      LOG_ERR("JSN", "safeWriteFile: failed to remove stale bak %s", bakPath);
    }
  }
  LOG_DIAG("JSN", "safeWriteFile step2 bak_exists_after=%d", Storage.exists(bakPath) ? 1 : 0);

  // 3. Rotate current to backup
  if (Storage.exists(path)) {
    if (!Storage.rename(path, bakPath)) {
      LOG_ERR("JSN", "safeWriteFile: failed to rotate %s to %s", path, bakPath);
      Storage.remove(activeTmp);
      return false;
    }
  }
  LOG_DIAG("JSN", "safeWriteFile step3 primary_exists_after=%d bak_exists_after=%d", Storage.exists(path) ? 1 : 0,
           Storage.exists(bakPath) ? 1 : 0);

  // 4. Promote tmp to current
  if (!Storage.rename(activeTmp, path)) {
    LOG_ERR("JSN", "safeWriteFile: failed to promote %s", activeTmp);
    if (Storage.exists(bakPath)) {
      if (Storage.exists(path)) {
        Storage.remove(path);
      }
      if (Storage.rename(bakPath, path)) {
        LOG_ERR("JSN", "safeWriteFile: restored %s from backup", path);
      } else {
        LOG_ERR("JSN", "safeWriteFile: failed to restore %s from backup %s", path, bakPath);
      }
    }
    return false;
  }
  LOG_DIAG("JSN", "safeWriteFile step4 promoted tmp->%s ok primary_exists=%d", path, Storage.exists(path) ? 1 : 0);

  return true;
}

// Read the JSON file, automatically trying fallbacks if a crash occurred
// mid-save
String JsonSettingsIO::safeReadFile(const char* path) {
  if (Storage.exists(path)) {
    String json = Storage.readFile(path);
    if (!json.isEmpty()) {
      LOG_DIAG("JSN", "safeReadFile: source=primary path=%s bytes=%u", path, (unsigned)json.length());
      return json;
    }
  }

  // Primary failed/empty. Try backup (which means crash happened during step 4
  // or 3)
  char bakPath[128];
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
  if (Storage.exists(bakPath)) {
    String json = Storage.readFile(bakPath);
    if (!json.isEmpty()) {
      LOG_DIAG("JSN", "safeReadFile: source=bak path=%s bytes=%u", bakPath, (unsigned)json.length());
      return json;
    }
  }

  // Backup failed/empty. Try tmp (which means crash happened during step 1 or
  // 2, but primary was also missing beforehand)
  char tmpPath[128];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  if (Storage.exists(tmpPath)) {
    String json = Storage.readFile(tmpPath);
    if (!json.isEmpty()) {
      LOG_DIAG("JSN", "safeReadFile: source=tmp path=%s bytes=%u", tmpPath, (unsigned)json.length());
      return json;
    }
  }

  LOG_DIAG("JSN", "safeReadFile: source=none path=%s (no readable source)", path);
  return "";
}

// ---- CrossPointSettings ----

bool JsonSettingsIO::saveSettings(const CrossPointSettings& s, const char* path) {
  LOG_DIAG("CPS", "saveSettings: enter ff=%u fs=%u lsp=%u customFont='%s' cfsPt=%u path=%s", (unsigned)s.fontFamily,
           (unsigned)s.fontSize, (unsigned)s.lineSpacingPercent, s.customFontName.c_str(), (unsigned)s.customFontSizePt,
           path);
  JsonDocument doc;

  doc["homeLayout"] = s.homeLayout;
  doc["sleepScreen"] = s.sleepScreen;
  doc["sleepScreenCoverMode"] = s.sleepScreenCoverMode;
  doc["sleepScreenCoverFilter"] = s.sleepScreenCoverFilter;
  doc["showSleepImageFilename"] = s.showSleepImageFilename;
  doc["statusBar"] = s.statusBar;
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
  doc["statusBarFontSize"] = s.statusBarFontSize;
  doc["statusBarBarThickness"] = s.statusBarBarThickness;
  doc["extraParagraphSpacingLevel"] = s.extraParagraphSpacingLevel;
  // Legacy compatibility key for older builds that still expect a toggle.
  doc["extraParagraphSpacing"] = s.extraParagraphSpacingLevel != CrossPointSettings::EXTRA_SPACING_OFF;
  doc["wordSpacingPercent"] = s.wordSpacingPercent;
  doc["firstLineIndentMode"] = s.firstLineIndentMode;
  doc["readerStyleMode"] = s.readerStyleMode;
  doc["textRenderMode"] = s.textRenderMode;
  doc["textRenderModeV2"] = true;
  doc["useFactoryLUT"] = s.useFactoryLUT;
  doc["shortPwrBtn"] = s.shortPwrBtn;
  doc["orientation"] = s.orientation;
  doc["sideButtonLayout"] = s.sideButtonLayout;
  doc["frontButtonBack"] = s.frontButtonBack;
  doc["frontButtonConfirm"] = s.frontButtonConfirm;
  doc["frontButtonLeft"] = s.frontButtonLeft;
  doc["frontButtonRight"] = s.frontButtonRight;
  doc["fontFamily"] = s.fontFamily;
  doc["fontSize"] = s.fontSize;
  doc["customFontName"] = s.customFontName;
  doc["customFontSizePt"] = s.customFontSizePt;
  doc["lineSpacing"] = s.lineSpacing;
  doc["lineSpacingPercent"] = s.lineSpacingPercent;
  doc["paragraphAlignment"] = s.paragraphAlignment;
  doc["sleepTimeout"] = s.sleepTimeout;
  doc["showHiddenFiles"] = s.showHiddenFiles;
  doc["randomBookOnBoot"] = s.randomBookOnBoot;
  doc["refreshFrequency"] = s.refreshFrequency;
  doc["screenMargin"] = s.screenMargin;
  doc["uniformMargins"] = s.uniformMargins;
  // Defensive clamp: field is 0/1/2 in the UI, but a bad in-memory value
  // would otherwise hit disk and force a reset on load.
  doc["dynamicMargins"] = static_cast<uint8_t>(s.dynamicMargins > 2 ? 0 : s.dynamicMargins);
  doc["screenMarginHorizontal"] = s.screenMarginHorizontal;
  doc["screenMarginTop"] = s.screenMarginTop;
  doc["screenMarginBottom"] = s.screenMarginBottom;
  doc["opdsServerUrl"] = s.opdsServerUrl;
  doc["opdsUsername"] = s.opdsUsername;
  doc["opdsPassword_obf"] = obfuscation::obfuscateToBase64(s.opdsPassword);
  doc["hideBatteryPercentage"] = s.hideBatteryPercentage;
  doc["longPressChapterSkip"] = s.longPressChapterSkip;
  doc["hyphenationEnabled"] = s.hyphenationEnabled;
  doc["uiLanguage"] = s.uiLanguage;
  doc["fadingFix"] = s.fadingFix;
  doc["embeddedStyle"] = s.readerStyleMode == CrossPointSettings::READER_STYLE_HYBRID;
  doc["debugBorders"] = s.debugBorders;
  doc["highlightMode"] = s.highlightMode;
  doc["darkMode"] = s.darkMode;
  doc["booksFolderOrder"] = s.booksFolderOrder;
  doc["imageDither"] = s.imageDither;

  String json;
  serializeJson(doc, json);
  const bool ok = safeWriteFile(path, json);
  LOG_DIAG("CPS", "saveSettings: exit ok=%d ff=%u fs=%u lsp=%u path=%s", ok ? 1 : 0, (unsigned)s.fontFamily,
           (unsigned)s.fontSize, (unsigned)s.lineSpacingPercent, path);
  return ok;
}

bool JsonSettingsIO::loadSettings(CrossPointSettings& s, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("CPS", "JSON parse error: %s", error.c_str());
    return false;
  }

  using S = CrossPointSettings;

  s.homeLayout =
      clampEnum(doc["homeLayout"] | (uint8_t)S::HOME_LAYOUT_CLASSIC, S::HOME_LAYOUT_COUNT, S::HOME_LAYOUT_CLASSIC);
  s.sleepScreen = clampEnum(doc["sleepScreen"] | (uint8_t)S::DARK, S::SLEEP_SCREEN_MODE_COUNT, S::DARK);
  s.sleepScreenCoverMode =
      clampEnum(doc["sleepScreenCoverMode"] | (uint8_t)S::FIT, S::SLEEP_SCREEN_COVER_MODE_COUNT, S::FIT);
  s.sleepScreenCoverFilter = clampEnum(doc["sleepScreenCoverFilter"] | (uint8_t)S::NO_FILTER,
                                       S::SLEEP_SCREEN_COVER_FILTER_COUNT, S::NO_FILTER);
  s.showSleepImageFilename = doc["showSleepImageFilename"] | (uint8_t)0;
  s.statusBar = clampEnum(doc["statusBar"] | (uint8_t)S::FULL, S::STATUS_BAR_MODE_COUNT, S::FULL);
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
    s.statusBarFontSize = clampEnum(doc["statusBarFontSize"] | (uint8_t)S::STATUS_FONT_SMALL,
                                    S::STATUS_BAR_FONT_SIZE_COUNT, S::STATUS_FONT_SMALL);
    s.statusBarBarThickness = clampEnum(doc["statusBarBarThickness"] | (uint8_t)S::STATUS_BAR_THICKNESS_NORMAL,
                                        S::STATUS_BAR_BAR_THICKNESS_COUNT, S::STATUS_BAR_THICKNESS_NORMAL);
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
    if (raw < S::WORD_SPACING_MODE_COUNT) {
      s.wordSpacingPercent = raw;
    } else {
      s.wordSpacingPercent = (uint8_t)S::WORD_SPACING_NORMAL;
      if (needsResave) *needsResave = true;
    }
  }
  s.firstLineIndentMode =
      clampEnum(doc["firstLineIndentMode"] | (uint8_t)S::INDENT_BOOK, S::FIRST_LINE_INDENT_MODE_COUNT, S::INDENT_BOOK);
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
    s.textRenderMode = (uint8_t)S::TEXT_RENDER_CRISP;
    if (needsResave) {
      *needsResave = true;
    }
  } else {
    // Migrate from v2 enum (Crisp=0, Light=1, Dark=2, ExtraDark=3) to v3 (Crisp=0, Dark=1, Bionic=2).
    const uint8_t raw = doc["textRenderMode"] | (uint8_t)S::TEXT_RENDER_CRISP;
    if (doc["textRenderModeV2"].isNull()) {
      // Old v2 format: any non-zero value → Dark
      s.textRenderMode = raw >= 1 ? (uint8_t)S::TEXT_RENDER_DARK : (uint8_t)S::TEXT_RENDER_CRISP;
    } else if (raw >= S::TEXT_RENDER_MODE_COUNT) {
      s.textRenderMode = (uint8_t)S::TEXT_RENDER_CRISP;
    } else {
      s.textRenderMode = raw;
    }
    if (needsResave && s.textRenderMode != raw) {
      *needsResave = true;
    }
  }
  s.textAntiAliasing = 0;
  s.useFactoryLUT = (doc["useFactoryLUT"] | 0) ? 1 : 0;
  setBitmapHelpersUseFactoryLUT(s.useFactoryLUT != 0);
  s.shortPwrBtn = clampEnum(doc["shortPwrBtn"] | (uint8_t)S::IGNORE, S::SHORT_PWRBTN_COUNT, S::IGNORE);
  s.orientation = clampEnum(doc["orientation"] | (uint8_t)S::PORTRAIT, S::ORIENTATION_COUNT, S::PORTRAIT);
  s.sideButtonLayout =
      clampEnum(doc["sideButtonLayout"] | (uint8_t)S::PREV_NEXT, S::SIDE_BUTTON_LAYOUT_COUNT, S::PREV_NEXT);
  s.frontButtonBack =
      clampEnum(doc["frontButtonBack"] | (uint8_t)S::FRONT_HW_BACK, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_BACK);
  s.frontButtonConfirm = clampEnum(doc["frontButtonConfirm"] | (uint8_t)S::FRONT_HW_CONFIRM,
                                   S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_CONFIRM);
  s.frontButtonLeft =
      clampEnum(doc["frontButtonLeft"] | (uint8_t)S::FRONT_HW_LEFT, S::FRONT_BUTTON_HARDWARE_COUNT, S::FRONT_HW_LEFT);
  s.frontButtonRight = clampEnum(doc["frontButtonRight"] | (uint8_t)S::FRONT_HW_RIGHT, S::FRONT_BUTTON_HARDWARE_COUNT,
                                 S::FRONT_HW_RIGHT);
  CrossPointSettings::validateFrontButtonMapping(s);
  const bool ffPresent = !doc["fontFamily"].isNull();
  const int ffRaw = ffPresent ? (int)(doc["fontFamily"] | (uint8_t)S::CHAREINK) : -1;
  s.fontFamily = clampEnum(doc["fontFamily"] | (uint8_t)S::CHAREINK, S::FONT_FAMILY_COUNT, S::CHAREINK);
  const uint8_t ffClamped = s.fontFamily;
  s.fontFamily = S::normalizeFontFamily(s.fontFamily);
  LOG_DIAG("CPS", "loadSettings: ff_present=%d ff_raw=%d ff_clamped=%u ff_normalized=%u", ffPresent ? 1 : 0, ffRaw,
           (unsigned)ffClamped, (unsigned)s.fontFamily);
  s.fontSize = clampEnum(doc["fontSize"] | (uint8_t)S::SIZE_16, S::FONT_SIZE_COUNT, S::SIZE_16);
  s.fontSize = S::normalizeFontSizeForFamily(s.fontFamily, s.fontSize);
  // customFontName is optional; absent → empty (old settings.json migrates
  // cleanly). No clamp/validation here — ReaderSettingsActivity and
  // getReaderFontId deal with stale/missing names at use time.
  s.customFontName = std::string(doc["customFontName"] | "");
  s.customFontSizePt = doc["customFontSizePt"] | (uint8_t)0;
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
  s.paragraphAlignment =
      clampEnum(doc["paragraphAlignment"] | (uint8_t)S::JUSTIFIED, S::PARAGRAPH_ALIGNMENT_COUNT, S::JUSTIFIED);
  s.sleepTimeout = clampEnum(doc["sleepTimeout"] | (uint8_t)S::SLEEP_10_MIN, S::SLEEP_TIMEOUT_COUNT, S::SLEEP_10_MIN);
  s.showHiddenFiles = doc["showHiddenFiles"] | (uint8_t)0;
  s.randomBookOnBoot = doc["randomBookOnBoot"] | (uint8_t)0;
  s.refreshFrequency =
      clampEnum(doc["refreshFrequency"] | (uint8_t)S::REFRESH_15, S::REFRESH_FREQUENCY_COUNT, S::REFRESH_15);
  s.screenMargin = doc["screenMargin"] | (uint8_t)5;
  s.uniformMargins = doc["uniformMargins"] | (uint8_t)0;
  if (s.uniformMargins > 1) s.uniformMargins = 0;
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
  s.hideBatteryPercentage =
      clampEnum(doc["hideBatteryPercentage"] | (uint8_t)S::HIDE_NEVER, S::HIDE_BATTERY_PERCENTAGE_COUNT, S::HIDE_NEVER);
  s.longPressChapterSkip = doc["longPressChapterSkip"] | (uint8_t)1;
  s.hyphenationEnabled = doc["hyphenationEnabled"] | (uint8_t)0;
  s.uiLanguage = clampEnum(doc["uiLanguage"] | (uint8_t)0, getLanguageCount(), 0);
  s.fadingFix = doc["fadingFix"] | (uint8_t)0;
  s.embeddedStyle = s.readerStyleMode == S::READER_STYLE_HYBRID ? (uint8_t)1 : (uint8_t)0;
  s.debugBorders = doc["debugBorders"] | (uint8_t)0;
  s.highlightMode = clampEnum(doc["highlightMode"] | (uint8_t)0, S::HIGHLIGHT_MODE_COUNT, 0);
  s.darkMode = doc["darkMode"] | (uint8_t)0;
  if (s.darkMode > 1) s.darkMode = 0;
  s.booksFolderOrder = doc["booksFolderOrder"] | (uint8_t)0;
  if (s.booksFolderOrder > 1) s.booksFolderOrder = 0;
  s.imageDither = clampEnum(doc["imageDither"] | (uint8_t)0, S::IMAGE_DITHER_COUNT, 0);

  const char* url = doc["opdsServerUrl"] | "";
  StringUtils::safeStrncpy(s.opdsServerUrl, url);

  const char* user = doc["opdsUsername"] | "";
  StringUtils::safeStrncpy(s.opdsUsername, user);

  bool passOk = false;
  std::string pass = obfuscation::deobfuscateFromBase64(doc["opdsPassword_obf"] | "", &passOk);
  if (!passOk || pass.empty()) {
    pass = doc["opdsPassword"] | "";
    if (!pass.empty() && needsResave) *needsResave = true;
  }
  StringUtils::safeStrncpy(s.opdsPassword, pass.c_str());
  LOG_DIAG("CPS", "loadSettings: ff=%u fs=%u lsp=%u customFont='%s' cfsPt=%u", (unsigned)s.fontFamily,
           (unsigned)s.fontSize, (unsigned)s.lineSpacingPercent, s.customFontName.c_str(),
           (unsigned)s.customFontSizePt);
  return true;
}

// ---- KOReaderCredentialStore ----

bool JsonSettingsIO::saveKOReader(const KOReaderCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["username"] = store.getUsername();
  doc["password_obf"] = obfuscation::obfuscateToBase64(store.getPassword());
  doc["serverUrl"] = store.getServerUrl();
  doc["matchMethod"] = static_cast<uint8_t>(store.getMatchMethod());

  String json;
  serializeJson(doc, json);
  return safeWriteFile(path, json);
}

bool JsonSettingsIO::loadKOReader(KOReaderCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("KRS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.username = doc["username"] | std::string("");
  bool ok = false;
  store.password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &ok);
  if (!ok || store.password.empty()) {
    store.password = doc["password"] | std::string("");
    if (!store.password.empty() && needsResave) *needsResave = true;
  }
  store.serverUrl = doc["serverUrl"] | std::string("");
  uint8_t method = doc["matchMethod"] | (uint8_t)0;
  store.matchMethod = static_cast<DocumentMatchMethod>(method);

  LOG_DBG("KRS", "Loaded KOReader credentials for user: %s", store.username.c_str());
  return true;
}

// ---- WifiCredentialStore ----

bool JsonSettingsIO::saveWifi(const WifiCredentialStore& store, const char* path) {
  JsonDocument doc;
  doc["lastConnectedSsid"] = store.getLastConnectedSsid();

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : store.getCredentials()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
  }

  String json;
  serializeJson(doc, json);
  return safeWriteFile(path, json);
}

bool JsonSettingsIO::loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave) {
  if (needsResave) *needsResave = false;
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WCS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.lastConnectedSsid = doc["lastConnectedSsid"] | std::string("");

  store.credentials.clear();
  JsonArray arr = doc["credentials"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.credentials.size() >= store.MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | std::string("");
    bool ok = false;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &ok);
    if (!ok || cred.password.empty()) {
      cred.password = obj["password"] | std::string("");
      if (!cred.password.empty() && needsResave) *needsResave = true;
    }
    store.credentials.push_back(cred);
  }

  LOG_DBG("WCS", "Loaded %zu WiFi credentials from file", store.credentials.size());
  return true;
}

// ---- RecentBooksStore ----

bool JsonSettingsIO::saveRecentBooks(const RecentBooksStore& store, const char* path) {
  JsonDocument doc;
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : store.getBooks()) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
    obj["boldSwap"] = book.boldSwap;
    obj["percent"] = book.percent;
  }

  String json;
  serializeJson(doc, json);
  return safeWriteFile(path, json);
}

bool JsonSettingsIO::loadRecentBooks(RecentBooksStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RBS", "JSON parse error: %s", error.c_str());
    return false;
  }

  store.recentBooks.clear();
  JsonArray arr = doc["books"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (store.getCount() >= RecentBooksStore::MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | std::string("");
    book.title = obj["title"] | std::string("");
    book.author = obj["author"] | std::string("");
    book.coverBmpPath = obj["coverBmpPath"] | std::string("");
    // Missing field (older recent.json) -> default OFF.
    book.boldSwap = (obj["boldSwap"] | (uint8_t)0) != 0 ? 1 : 0;
    // Missing field (older recent.json) -> -1 = unknown; first read while
    // book is open will populate it.
    book.percent = static_cast<int8_t>(obj["percent"] | -1);
    store.recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", store.getCount());
  return true;
}

// ---- ReadingThemeStore ----

bool JsonSettingsIO::saveReadingTheme(const ReadingTheme& theme, const char* path) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  writeReadingThemeObject(obj, theme);

  String json;
  serializeJson(doc, json);
  return safeWriteFile(path, json);
}

bool JsonSettingsIO::loadReadingTheme(ReadingTheme& theme, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RTH", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonObject obj = doc.as<JsonObject>();
  if (obj.isNull()) {
    LOG_ERR("RTH", "Missing reading theme object");
    return false;
  }

  readReadingThemeObject(obj, theme);
  return true;
}

bool JsonSettingsIO::saveReadingThemes(const ReadingThemeStore& store, const char* path) {
  JsonDocument doc;
  doc["lastEditedThemeIndex"] = store.getLastEditedThemeIndex();
  JsonArray arr = doc["themes"].to<JsonArray>();
  for (const auto& theme : store.getThemes()) {
    JsonObject obj = arr.add<JsonObject>();
    writeReadingThemeObject(obj, theme);
  }

  String json;
  serializeJson(doc, json);
  return safeWriteFile(path, json);
}

bool JsonSettingsIO::loadReadingThemes(ReadingThemeStore& store, const char* json) {
  JsonDocument doc;
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RTH", "JSON parse error: %s", error.c_str());
    return false;
  }

  // Parse into a temporary list first — never clear in-memory themes until we
  // know the file actually contained data.  This prevents a valid-but-empty
  // JSON (e.g. truncated write, SD corruption) from wiping the user's themes.
  std::vector<ReadingTheme> parsed;
  JsonArray arr = doc["themes"].as<JsonArray>();
  for (JsonObject obj : arr) {
    if (parsed.size() >= ReadingThemeStore::MAX_THEMES) {
      break;
    }
    ReadingTheme theme;
    readReadingThemeObject(obj, theme);
    parsed.push_back(theme);
  }

  if (parsed.empty() && !store.themes.empty()) {
    LOG_ERR("RTH",
            "Parsed 0 themes from JSON but %u in memory — rejecting load "
            "(possible file corruption)",
            (unsigned)store.themes.size());
    return false;
  }

  store.themes = std::move(parsed);
  store.lastEditedThemeIndex = doc["lastEditedThemeIndex"] | -1;
  if (store.lastEditedThemeIndex < 0 || store.lastEditedThemeIndex >= static_cast<int>(store.themes.size())) {
    store.lastEditedThemeIndex = -1;
  }

  return true;
}
