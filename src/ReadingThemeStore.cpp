#include "ReadingThemeStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>

#include "Paths.h"

namespace {
constexpr char READING_THEMES_FILE_JSON[] = "/.crosspoint/reading_themes.json";
constexpr char BOOK_READER_SETTINGS_FILE_JSON[] = "/reader_settings.json";

uint8_t clampRange(const uint8_t value, const uint8_t minValue,
                   const uint8_t maxValue, const uint8_t defaultValue) {
  if (value < minValue || value > maxValue) {
    return defaultValue;
  }
  return value;
}

bool sameThemeName(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (size_t i = 0; i < left.size(); i++) {
    const char a = static_cast<char>(
        std::tolower(static_cast<unsigned char>(left[i])));
    const char b = static_cast<char>(
        std::tolower(static_cast<unsigned char>(right[i])));
    if (a != b) {
      return false;
    }
  }
  return true;
}

std::string bookReaderSettingsPath(const std::string& cachePath) {
  return cachePath + BOOK_READER_SETTINGS_FILE_JSON;
}

bool persistAppliedSettings(const std::string& cachePath) {
  if (cachePath.empty()) {
    // No book context — save to global settings only
    return SETTINGS.saveToFile();
  }

  // In-book context — save per-book only, keep global as new-book defaults
  return ReadingThemeStore::saveCurrentBookSettings(cachePath);
}
}  // namespace

ReadingThemeStore ReadingThemeStore::instance;

const ReadingTheme* ReadingThemeStore::getTheme(const size_t index) const {
  if (index >= themes.size()) {
    return nullptr;
  }
  return &themes[index];
}

bool ReadingThemeStore::saveToFile() const {
  // Guard 1: never loaded successfully — refuse to save (transient SD failure)
  if (!loadedFromFile && themes.empty()) {
    LOG_ERR("RTH",
            "saveToFile blocked: load never succeeded and themes is empty — "
            "refusing to overwrite file");
    return false;
  }

  // Guard 2: themes were loaded but are now empty.  The only legitimate path
  // to an empty list is deleting the very last theme one-by-one, which is fine.
  // But if we somehow ended up empty through a load bug, we'd silently wipe
  // the user's .bak on the next save cycle.  Log a warning so it's visible.
  if (loadedFromFile && themes.empty()) {
    LOG_INF("RTH", "saveToFile: saving empty theme list (all themes deleted)");
  }

  Storage.mkdir(Paths::kDataDir);
  return JsonSettingsIO::saveReadingThemes(*this, READING_THEMES_FILE_JSON);
}

bool ReadingThemeStore::loadFromFile() {
  const String json = JsonSettingsIO::safeReadFile(READING_THEMES_FILE_JSON);
  if (json.isEmpty()) {
    LOG_ERR("RTH", "loadFromFile: no readable theme file found");
    return false;
  }
  if (!JsonSettingsIO::loadReadingThemes(*this, json.c_str())) {
    // Primary parse failed or returned 0 themes while we had some in memory.
    // Try the .bak file explicitly as a recovery path.
    LOG_ERR("RTH", "loadFromFile: primary parse rejected — trying .bak");
    char bakPath[128];
    snprintf(bakPath, sizeof(bakPath), "%s.bak", READING_THEMES_FILE_JSON);
    const String bakJson = JsonSettingsIO::safeReadFile(bakPath);
    if (bakJson.isEmpty() ||
        !JsonSettingsIO::loadReadingThemes(*this, bakJson.c_str())) {
      LOG_ERR("RTH", "loadFromFile: .bak recovery also failed");
      return false;
    }
    LOG_INF("RTH", "loadFromFile: recovered %d themes from .bak",
            (int)themes.size());
  }
  loadedFromFile = true;
  return true;
}

ReadingTheme ReadingThemeStore::fromSettings(const std::string& name,
                                             const CrossPointSettings& settings) {
  ReadingTheme theme;
  theme.name = sanitizeName(name);
  theme.fontFamily = settings.fontFamily;
  theme.fontSize = settings.fontSize;
  theme.lineSpacingPercent = settings.lineSpacingPercent;
  theme.uniformMargins = settings.uniformMargins;
  theme.dynamicMargins = settings.dynamicMargins;
  theme.screenMarginHorizontal = settings.screenMarginHorizontal;
  theme.screenMarginTop = settings.screenMarginTop;
  theme.screenMarginBottom = settings.screenMarginBottom;
  theme.paragraphAlignment = settings.paragraphAlignment;
  theme.extraParagraphSpacingLevel = settings.extraParagraphSpacingLevel;
  theme.wordSpacingPercent = settings.wordSpacingPercent;
  theme.firstLineIndentMode = settings.firstLineIndentMode;
  theme.readerStyleMode = settings.readerStyleMode;
  theme.textRenderMode = settings.textRenderMode;
  theme.hyphenationEnabled = settings.hyphenationEnabled;
  theme.statusBarEnabled = settings.statusBarEnabled;
  theme.statusBarShowBattery = settings.statusBarShowBattery;
  theme.statusBarShowPageCounter = settings.statusBarShowPageCounter;
  theme.statusBarPageCounterMode = settings.statusBarPageCounterMode;
  theme.statusBarShowBookPercentage = settings.statusBarShowBookPercentage;
  theme.statusBarShowChapterPercentage = settings.statusBarShowChapterPercentage;
  theme.statusBarShowBookBar = settings.statusBarShowBookBar;
  theme.statusBarShowChapterBar = settings.statusBarShowChapterBar;
  theme.statusBarShowChapterTitle = settings.statusBarShowChapterTitle;
  theme.statusBarNoTitleTruncation = settings.statusBarNoTitleTruncation;
  theme.statusBarBatteryPosition = settings.statusBarBatteryPosition;
  theme.statusBarProgressTextPosition = settings.statusBarProgressTextPosition;
  theme.statusBarPageCounterPosition = settings.statusBarPageCounterPosition;
  theme.statusBarBookPercentagePosition =
      settings.statusBarBookPercentagePosition;
  theme.statusBarChapterPercentagePosition =
      settings.statusBarChapterPercentagePosition;
  theme.statusBarBookBarPosition = settings.statusBarBookBarPosition;
  theme.statusBarChapterBarPosition = settings.statusBarChapterBarPosition;
  theme.statusBarTitlePosition = settings.statusBarTitlePosition;
  theme.statusBarTextAlignment = settings.statusBarTextAlignment;
  theme.statusBarProgressStyle = settings.statusBarProgressStyle;
  theme.statusBarFontSize = settings.statusBarFontSize;
  theme.statusBarBarThickness = settings.statusBarBarThickness;
  theme.statusBarShowBookPageCounter = settings.statusBarShowBookPageCounter;
  theme.statusBarBookPageCounterPosition =
      settings.statusBarBookPageCounterPosition;
  return theme;
}

ReadingTheme ReadingThemeStore::captureCurrent(const std::string& name) const {
  return fromSettings(name, SETTINGS);
}

void ReadingThemeStore::applyThemeToSettings(const ReadingTheme& theme,
                                             CrossPointSettings& settings) {
  settings.fontFamily =
      CrossPointSettings::normalizeFontFamily(theme.fontFamily);
  settings.fontSize = CrossPointSettings::normalizeFontSizeForFamily(
      settings.fontFamily, theme.fontSize);
  settings.lineSpacingPercent =
      clampRange(theme.lineSpacingPercent, 65, 150, 110);
  settings.uniformMargins = theme.uniformMargins ? 1 : 0;
  settings.dynamicMargins = theme.dynamicMargins ? 1 : 0;
  settings.screenMarginHorizontal =
      clampRange(theme.screenMarginHorizontal, 0, 55, 20);
  settings.screenMarginTop = clampRange(theme.screenMarginTop, 0, 55, 20);
  settings.screenMarginBottom =
      clampRange(theme.screenMarginBottom, 0, 55, 20);
  settings.paragraphAlignment = clampRange(
      theme.paragraphAlignment, 0,
      CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1,
      CrossPointSettings::JUSTIFIED);
  settings.extraParagraphSpacingLevel = clampRange(
      theme.extraParagraphSpacingLevel, 0,
      CrossPointSettings::EXTRA_PARAGRAPH_SPACING_COUNT - 1,
      CrossPointSettings::EXTRA_SPACING_M);
  settings.wordSpacingPercent = clampRange(
      theme.wordSpacingPercent, 0,
      CrossPointSettings::WORD_SPACING_MODE_COUNT - 1,
      CrossPointSettings::WORD_SPACING_NORMAL);
  settings.firstLineIndentMode = clampRange(
      theme.firstLineIndentMode, 0,
      CrossPointSettings::FIRST_LINE_INDENT_MODE_COUNT - 1,
      CrossPointSettings::INDENT_BOOK);
  settings.readerStyleMode = clampRange(
      theme.readerStyleMode, 0,
      CrossPointSettings::READER_STYLE_MODE_COUNT - 1,
      CrossPointSettings::READER_STYLE_USER);
  settings.textRenderMode = clampRange(
      theme.textRenderMode, 0,
      CrossPointSettings::TEXT_RENDER_MODE_COUNT - 1,
      CrossPointSettings::TEXT_RENDER_CRISP);
  settings.embeddedStyle =
      settings.readerStyleMode == CrossPointSettings::READER_STYLE_HYBRID ? 1 : 0;
  settings.textAntiAliasing = 0;
  settings.hyphenationEnabled = theme.hyphenationEnabled ? 1 : 0;
  settings.statusBarEnabled = theme.statusBarEnabled ? 1 : 0;
  settings.statusBarShowBattery = theme.statusBarShowBattery ? 1 : 0;
  settings.statusBarShowPageCounter = theme.statusBarShowPageCounter ? 1 : 0;
  settings.statusBarPageCounterMode =
      CrossPointSettings::normalizeStatusBarPageCounterMode(
          theme.statusBarPageCounterMode);
  settings.statusBarShowBookPercentage =
      theme.statusBarShowBookPercentage ? 1 : 0;
  settings.statusBarShowChapterPercentage =
      theme.statusBarShowChapterPercentage ? 1 : 0;
  settings.statusBarShowBookBar = theme.statusBarShowBookBar ? 1 : 0;
  settings.statusBarShowChapterBar = theme.statusBarShowChapterBar ? 1 : 0;
  settings.statusBarShowChapterTitle = theme.statusBarShowChapterTitle ? 1 : 0;
  settings.statusBarNoTitleTruncation =
      theme.statusBarNoTitleTruncation ? 1 : 0;
  settings.statusBarBatteryPosition = clampRange(
      theme.statusBarBatteryPosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT);
  settings.statusBarProgressTextPosition = clampRange(
      theme.statusBarProgressTextPosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  settings.statusBarPageCounterPosition = clampRange(
      theme.statusBarPageCounterPosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  settings.statusBarBookPercentagePosition = clampRange(
      theme.statusBarBookPercentagePosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  settings.statusBarChapterPercentagePosition = clampRange(
      theme.statusBarChapterPercentagePosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  settings.statusBarBookBarPosition = clampRange(
      theme.statusBarBookBarPosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  settings.statusBarChapterBarPosition = clampRange(
      theme.statusBarChapterBarPosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  settings.statusBarTitlePosition = clampRange(
      theme.statusBarTitlePosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  settings.statusBarTextAlignment = clampRange(
      theme.statusBarTextAlignment, 0,
      CrossPointSettings::STATUS_TEXT_ALIGNMENT_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_RIGHT);
  settings.statusBarProgressStyle = clampRange(
      theme.statusBarProgressStyle, 0,
      CrossPointSettings::STATUS_BAR_PROGRESS_STYLE_COUNT - 1,
      CrossPointSettings::STATUS_BAR_THICK);
  settings.statusBarFontSize = clampRange(
      theme.statusBarFontSize, 0,
      CrossPointSettings::STATUS_BAR_FONT_SIZE_COUNT - 1,
      CrossPointSettings::STATUS_FONT_SMALL);
  settings.statusBarBarThickness = clampRange(
      theme.statusBarBarThickness, 0,
      CrossPointSettings::STATUS_BAR_BAR_THICKNESS_COUNT - 1,
      CrossPointSettings::STATUS_BAR_THICKNESS_NORMAL);
  settings.statusBarShowBookPageCounter =
      theme.statusBarShowBookPageCounter ? 1 : 0;
  settings.statusBarBookPageCounterPosition = clampRange(
      theme.statusBarBookPageCounterPosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
}

bool ReadingThemeStore::matchesCurrent(const ReadingTheme& theme) const {
  ReadingTheme current = fromSettings("", SETTINGS);
  return current.fontFamily == theme.fontFamily &&
         current.fontSize == theme.fontSize &&
         current.lineSpacingPercent == theme.lineSpacingPercent &&
         current.uniformMargins == theme.uniformMargins &&
         current.dynamicMargins == theme.dynamicMargins &&
         current.screenMarginHorizontal == theme.screenMarginHorizontal &&
         current.screenMarginTop == theme.screenMarginTop &&
         current.screenMarginBottom == theme.screenMarginBottom &&
         current.paragraphAlignment == theme.paragraphAlignment &&
         current.extraParagraphSpacingLevel ==
             theme.extraParagraphSpacingLevel &&
         current.wordSpacingPercent == theme.wordSpacingPercent &&
         current.firstLineIndentMode == theme.firstLineIndentMode &&
         current.readerStyleMode == theme.readerStyleMode &&
         current.textRenderMode == theme.textRenderMode &&
         current.hyphenationEnabled == theme.hyphenationEnabled &&
         current.statusBarEnabled == theme.statusBarEnabled &&
         current.statusBarShowBattery == theme.statusBarShowBattery &&
         current.statusBarShowPageCounter == theme.statusBarShowPageCounter &&
         current.statusBarPageCounterMode == theme.statusBarPageCounterMode &&
         current.statusBarShowBookPercentage ==
             theme.statusBarShowBookPercentage &&
         current.statusBarShowChapterPercentage ==
             theme.statusBarShowChapterPercentage &&
         current.statusBarShowBookBar == theme.statusBarShowBookBar &&
         current.statusBarShowChapterBar == theme.statusBarShowChapterBar &&
         current.statusBarShowChapterTitle ==
             theme.statusBarShowChapterTitle &&
         current.statusBarNoTitleTruncation ==
             theme.statusBarNoTitleTruncation &&
         current.statusBarBatteryPosition ==
             theme.statusBarBatteryPosition &&
         current.statusBarProgressTextPosition ==
             theme.statusBarProgressTextPosition &&
         current.statusBarPageCounterPosition ==
             theme.statusBarPageCounterPosition &&
         current.statusBarBookPercentagePosition ==
             theme.statusBarBookPercentagePosition &&
         current.statusBarChapterPercentagePosition ==
             theme.statusBarChapterPercentagePosition &&
         current.statusBarBookBarPosition == theme.statusBarBookBarPosition &&
         current.statusBarChapterBarPosition ==
             theme.statusBarChapterBarPosition &&
         current.statusBarTitlePosition == theme.statusBarTitlePosition &&
         current.statusBarTextAlignment == theme.statusBarTextAlignment &&
         current.statusBarProgressStyle == theme.statusBarProgressStyle &&
         current.statusBarFontSize == theme.statusBarFontSize &&
         current.statusBarBarThickness == theme.statusBarBarThickness &&
         current.statusBarShowBookPageCounter ==
             theme.statusBarShowBookPageCounter &&
         current.statusBarBookPageCounterPosition ==
             theme.statusBarBookPageCounterPosition;
}

int ReadingThemeStore::findMatchingTheme() const {
  for (size_t i = 0; i < themes.size(); i++) {
    if (matchesCurrent(themes[i])) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int ReadingThemeStore::findLastAppliedTheme() const {
  if (lastAppliedThemeName.empty()) {
    return -1;
  }
  for (size_t i = 0; i < themes.size(); i++) {
    if (sameThemeName(themes[i].name, lastAppliedThemeName)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::string ReadingThemeStore::sanitizeName(const std::string& name) {
  std::string trimmed;
  trimmed.reserve(name.size());
  bool seenNonSpace = false;
  for (char c : name) {
    const bool isSpace = std::isspace(static_cast<unsigned char>(c)) != 0;
    if (!seenNonSpace && isSpace) {
      continue;
    }
    seenNonSpace = true;
    trimmed.push_back(c);
  }
  while (!trimmed.empty() &&
         std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
    trimmed.pop_back();
  }
  if (trimmed.empty()) {
    trimmed = "Theme";
  }
  if (trimmed.size() > MAX_THEME_NAME_LENGTH) {
    trimmed.resize(MAX_THEME_NAME_LENGTH);
  }
  return trimmed;
}

std::string ReadingThemeStore::makeUniqueName(const std::string& desiredName,
                                              const int ignoreIndex) const {
  const std::string base = sanitizeName(desiredName);
  std::string candidate = base;
  int suffix = 2;
  auto conflicts = [&](const std::string& name) {
    for (size_t i = 0; i < themes.size(); i++) {
      if (static_cast<int>(i) == ignoreIndex) {
        continue;
      }
      if (sameThemeName(themes[i].name, name)) {
        return true;
      }
    }
    return false;
  };

  while (conflicts(candidate)) {
    const std::string suffixText = " " + std::to_string(suffix);
    candidate = base;
    if (candidate.size() + suffixText.size() > MAX_THEME_NAME_LENGTH) {
      candidate.resize(MAX_THEME_NAME_LENGTH - suffixText.size());
    }
    candidate += suffixText;
    suffix++;
  }
  return candidate;
}

bool ReadingThemeStore::addTheme(const std::string& name) {
  if (themes.size() >= MAX_THEMES) {
    return false;
  }
  themes.push_back(captureCurrent(makeUniqueName(name)));
  lastEditedThemeIndex = static_cast<int>(themes.size()) - 1;
  return saveToFile();
}

bool ReadingThemeStore::updateTheme(const size_t index) {
  if (index >= themes.size()) {
    return false;
  }
  themes[index] = captureCurrent(themes[index].name);
  lastEditedThemeIndex = static_cast<int>(index);
  return saveToFile();
}

bool ReadingThemeStore::renameTheme(const size_t index,
                                    const std::string& desiredName) {
  if (index >= themes.size()) {
    return false;
  }
  themes[index].name = makeUniqueName(desiredName, static_cast<int>(index));
  lastEditedThemeIndex = static_cast<int>(index);
  return saveToFile();
}

bool ReadingThemeStore::deleteTheme(const size_t index) {
  if (index >= themes.size()) {
    return false;
  }
  themes.erase(themes.begin() + static_cast<long>(index));
  if (lastEditedThemeIndex == static_cast<int>(index)) {
    lastEditedThemeIndex = -1;
  } else if (lastEditedThemeIndex > static_cast<int>(index)) {
    lastEditedThemeIndex--;
  }
  return saveToFile();
}

void ReadingThemeStore::sortByName() {
  std::sort(themes.begin(), themes.end(),
            [](const ReadingTheme& a, const ReadingTheme& b) {
              // Case-insensitive comparison
              const size_t len = std::min(a.name.size(), b.name.size());
              for (size_t i = 0; i < len; i++) {
                const char ca = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(a.name[i])));
                const char cb = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(b.name[i])));
                if (ca != cb) return ca < cb;
              }
              return a.name.size() < b.name.size();
            });
  lastEditedThemeIndex = -1;
}

bool ReadingThemeStore::applyTheme(const size_t index,
                                   const std::string& cachePath) {
  if (index >= themes.size()) {
    return false;
  }

  const ReadingTheme previous = fromSettings("", SETTINGS);
  applyThemeToSettings(themes[index], SETTINGS);
  if (!persistAppliedSettings(cachePath)) {
    applyThemeToSettings(previous, SETTINGS);
    persistAppliedSettings(cachePath);
    return false;
  }

  revertTheme = previous;
  hasRevertTheme = true;
  revertThemeCachePath = cachePath;
  lastAppliedThemeName = themes[index].name;
  return true;
}

bool ReadingThemeStore::canRevertTheme(const std::string& cachePath) const {
  return hasRevertTheme &&
         (revertThemeCachePath.empty() || revertThemeCachePath == cachePath);
}

bool ReadingThemeStore::revertThemeChange(const std::string& cachePath) {
  if (!canRevertTheme(cachePath)) {
    return false;
  }

  const ReadingTheme current = fromSettings("", SETTINGS);
  applyThemeToSettings(revertTheme, SETTINGS);
  if (!persistAppliedSettings(cachePath)) {
    applyThemeToSettings(current, SETTINGS);
    persistAppliedSettings(cachePath);
    return false;
  }

  hasRevertTheme = false;
  revertThemeCachePath.clear();
  return true;
}

bool ReadingThemeStore::saveCurrentBookSettings(const std::string& cachePath) {
  if (cachePath.empty()) {
    return false;
  }
  return JsonSettingsIO::saveReadingTheme(
      fromSettings(instance.lastAppliedThemeName, SETTINGS),
      bookReaderSettingsPath(cachePath).c_str());
}

bool ReadingThemeStore::loadBookSettings(const std::string& cachePath,
                                         ReadingTheme& theme) {
  if (cachePath.empty()) {
    return false;
  }

  const String json =
      JsonSettingsIO::safeReadFile(bookReaderSettingsPath(cachePath).c_str());
  if (json.isEmpty()) {
    return false;
  }

  return JsonSettingsIO::loadReadingTheme(theme, json.c_str());
}

bool ReadingThemeStore::loadBookSettingsIntoCurrent(
    const std::string& cachePath) {
  ReadingTheme theme;
  if (!loadBookSettings(cachePath, theme)) {
    return false;
  }

  applyThemeToSettings(theme, SETTINGS);
  instance.lastAppliedThemeName = theme.name;
  return true;
}

ReadingTheme ReadingThemeStore::normalizeTheme(const ReadingTheme& theme) {
  ReadingTheme normalized = theme;
  normalized.name = sanitizeName(theme.name);
  normalized.fontFamily =
      CrossPointSettings::normalizeFontFamily(theme.fontFamily);
  normalized.fontSize = CrossPointSettings::normalizeFontSizeForFamily(
      normalized.fontFamily, theme.fontSize);
  normalized.lineSpacingPercent =
      clampRange(theme.lineSpacingPercent, 65, 150, 110);
  normalized.uniformMargins = theme.uniformMargins ? 1 : 0;
  normalized.dynamicMargins = theme.dynamicMargins ? 1 : 0;
  normalized.screenMarginHorizontal =
      clampRange(theme.screenMarginHorizontal, 0, 55, 20);
  normalized.screenMarginTop = clampRange(theme.screenMarginTop, 0, 55, 20);
  normalized.screenMarginBottom =
      clampRange(theme.screenMarginBottom, 0, 55, 20);
  normalized.paragraphAlignment = clampRange(
      theme.paragraphAlignment, 0,
      CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1,
      CrossPointSettings::JUSTIFIED);
  normalized.extraParagraphSpacingLevel = clampRange(
      theme.extraParagraphSpacingLevel, 0,
      CrossPointSettings::EXTRA_PARAGRAPH_SPACING_COUNT - 1,
      CrossPointSettings::EXTRA_SPACING_M);
  normalized.wordSpacingPercent = clampRange(
      theme.wordSpacingPercent, 0,
      CrossPointSettings::WORD_SPACING_MODE_COUNT - 1,
      CrossPointSettings::WORD_SPACING_NORMAL);
  normalized.firstLineIndentMode = clampRange(
      theme.firstLineIndentMode, 0,
      CrossPointSettings::FIRST_LINE_INDENT_MODE_COUNT - 1,
      CrossPointSettings::INDENT_BOOK);
  normalized.readerStyleMode = clampRange(
      theme.readerStyleMode, 0,
      CrossPointSettings::READER_STYLE_MODE_COUNT - 1,
      CrossPointSettings::READER_STYLE_USER);
  normalized.textRenderMode = clampRange(
      theme.textRenderMode, 0,
      CrossPointSettings::TEXT_RENDER_MODE_COUNT - 1,
      CrossPointSettings::TEXT_RENDER_CRISP);
  normalized.hyphenationEnabled = theme.hyphenationEnabled ? 1 : 0;
  normalized.statusBarEnabled = theme.statusBarEnabled ? 1 : 0;
  normalized.statusBarShowBattery = theme.statusBarShowBattery ? 1 : 0;
  normalized.statusBarShowPageCounter = theme.statusBarShowPageCounter ? 1 : 0;
  normalized.statusBarPageCounterMode =
      CrossPointSettings::normalizeStatusBarPageCounterMode(
          theme.statusBarPageCounterMode);
  normalized.statusBarShowBookPercentage =
      theme.statusBarShowBookPercentage ? 1 : 0;
  normalized.statusBarShowChapterPercentage =
      theme.statusBarShowChapterPercentage ? 1 : 0;
  normalized.statusBarShowBookBar = theme.statusBarShowBookBar ? 1 : 0;
  normalized.statusBarShowChapterBar = theme.statusBarShowChapterBar ? 1 : 0;
  normalized.statusBarShowChapterTitle =
      theme.statusBarShowChapterTitle ? 1 : 0;
  normalized.statusBarNoTitleTruncation =
      theme.statusBarNoTitleTruncation ? 1 : 0;
  normalized.statusBarBatteryPosition = clampRange(
      theme.statusBarBatteryPosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT);
  normalized.statusBarProgressTextPosition = clampRange(
      theme.statusBarProgressTextPosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  normalized.statusBarPageCounterPosition = clampRange(
      theme.statusBarPageCounterPosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  normalized.statusBarBookPercentagePosition = clampRange(
      theme.statusBarBookPercentagePosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  normalized.statusBarChapterPercentagePosition = clampRange(
      theme.statusBarChapterPercentagePosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  normalized.statusBarBookBarPosition = clampRange(
      theme.statusBarBookBarPosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  normalized.statusBarChapterBarPosition = clampRange(
      theme.statusBarChapterBarPosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  normalized.statusBarTitlePosition = clampRange(
      theme.statusBarTitlePosition, 0,
      CrossPointSettings::STATUS_BAR_ITEM_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_AT_BOTTOM);
  normalized.statusBarTextAlignment = clampRange(
      theme.statusBarTextAlignment, 0,
      CrossPointSettings::STATUS_TEXT_ALIGNMENT_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_RIGHT);
  normalized.statusBarProgressStyle = clampRange(
      theme.statusBarProgressStyle, 0,
      CrossPointSettings::STATUS_BAR_PROGRESS_STYLE_COUNT - 1,
      CrossPointSettings::STATUS_BAR_THICK);
  normalized.statusBarFontSize = clampRange(
      theme.statusBarFontSize, 0,
      CrossPointSettings::STATUS_BAR_FONT_SIZE_COUNT - 1,
      CrossPointSettings::STATUS_FONT_SMALL);
  normalized.statusBarBarThickness = clampRange(
      theme.statusBarBarThickness, 0,
      CrossPointSettings::STATUS_BAR_BAR_THICKNESS_COUNT - 1,
      CrossPointSettings::STATUS_BAR_THICKNESS_NORMAL);
  normalized.statusBarShowBookPageCounter =
      theme.statusBarShowBookPageCounter ? 1 : 0;
  normalized.statusBarBookPageCounterPosition = clampRange(
      theme.statusBarBookPageCounterPosition, 0,
      CrossPointSettings::STATUS_BAR_TEXT_POSITION_COUNT - 1,
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER);
  return normalized;
}
