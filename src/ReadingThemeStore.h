#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"

struct ReadingTheme {
  std::string name;
  uint8_t fontFamily = CrossPointSettings::CHAREINK;
  uint8_t fontSize = CrossPointSettings::SIZE_16;
  uint8_t lineSpacingPercent = 110;
  uint8_t uniformMargins = 0;
  uint8_t dynamicMargins = 0;
  uint8_t screenMarginHorizontal = 20;
  uint8_t screenMarginTop = 20;
  uint8_t screenMarginBottom = 20;
  uint8_t paragraphAlignment = CrossPointSettings::JUSTIFIED;
  uint8_t extraParagraphSpacingLevel = CrossPointSettings::EXTRA_SPACING_M;
  uint8_t wordSpacingPercent = CrossPointSettings::WORD_SPACING_NORMAL;
  uint8_t firstLineIndentMode = CrossPointSettings::INDENT_BOOK;
  uint8_t readerStyleMode = CrossPointSettings::READER_STYLE_USER;
  uint8_t textRenderMode = CrossPointSettings::TEXT_RENDER_CRISP;
  uint8_t hyphenationEnabled = 0;
  uint8_t statusBarEnabled = 1;
  uint8_t statusBarShowBattery = 1;
  uint8_t statusBarShowPageCounter = 0;
  uint8_t statusBarPageCounterMode =
      CrossPointSettings::STATUS_PAGE_CURRENT_OVER_TOTAL;
  uint8_t statusBarShowBookPercentage = 0;
  uint8_t statusBarShowChapterPercentage = 0;
  uint8_t statusBarShowBookBar = 0;
  uint8_t statusBarShowChapterBar = 0;
  uint8_t statusBarShowChapterTitle = 1;
  uint8_t statusBarNoTitleTruncation = 0;
  uint8_t statusBarBatteryPosition = CrossPointSettings::STATUS_TEXT_BOTTOM_LEFT;
  uint8_t statusBarProgressTextPosition = CrossPointSettings::STATUS_AT_BOTTOM;
  uint8_t statusBarPageCounterPosition =
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  uint8_t statusBarBookPercentagePosition =
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  uint8_t statusBarChapterPercentagePosition =
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
  uint8_t statusBarBookBarPosition = CrossPointSettings::STATUS_AT_BOTTOM;
  uint8_t statusBarChapterBarPosition = CrossPointSettings::STATUS_AT_BOTTOM;
  uint8_t statusBarTitlePosition = CrossPointSettings::STATUS_AT_BOTTOM;
  uint8_t statusBarTextAlignment = CrossPointSettings::STATUS_TEXT_RIGHT;
  uint8_t statusBarProgressStyle = CrossPointSettings::STATUS_BAR_THICK;
  uint8_t statusBarFontSize = CrossPointSettings::STATUS_FONT_SMALL;
  uint8_t statusBarBarThickness = CrossPointSettings::STATUS_BAR_THICKNESS_NORMAL;
  uint8_t statusBarShowBookPageCounter = 0;
  uint8_t statusBarBookPageCounterPosition =
      CrossPointSettings::STATUS_TEXT_BOTTOM_CENTER;
};

class ReadingThemeStore;
namespace JsonSettingsIO {
bool saveReadingThemes(const ReadingThemeStore& store, const char* path);
bool loadReadingThemes(ReadingThemeStore& store, const char* json);
}  // namespace JsonSettingsIO

class ReadingThemeStore {
  static ReadingThemeStore instance;

  std::vector<ReadingTheme> themes;
  int lastEditedThemeIndex = -1;
  ReadingTheme revertTheme;
  bool hasRevertTheme = false;
  bool loadedFromFile = false;  // true once loadFromFile() succeeds
  std::string revertThemeCachePath;
  std::string lastAppliedThemeName;

  friend bool JsonSettingsIO::loadReadingThemes(ReadingThemeStore& store,
                                                const char* json);

 public:
  static constexpr size_t MAX_THEMES = 16;
  static constexpr size_t MAX_THEME_NAME_LENGTH = 20;

  static ReadingThemeStore& getInstance() { return instance; }

  const std::vector<ReadingTheme>& getThemes() const { return themes; }
  const ReadingTheme* getTheme(size_t index) const;
  int getCount() const { return static_cast<int>(themes.size()); }
  bool isEmpty() const { return themes.empty(); }
  int getLastEditedThemeIndex() const { return lastEditedThemeIndex; }
  const std::string& getLastAppliedThemeName() const { return lastAppliedThemeName; }
  int findLastAppliedTheme() const;

  bool saveToFile() const;
  bool loadFromFile();

  ReadingTheme captureCurrent(const std::string& name) const;
  bool matchesCurrent(const ReadingTheme& theme) const;
  int findMatchingTheme() const;

  std::string makeUniqueName(const std::string& desiredName,
                             int ignoreIndex = -1) const;
  bool addTheme(const std::string& name);
  bool updateTheme(size_t index);
  bool renameTheme(size_t index, const std::string& desiredName);
  bool deleteTheme(size_t index);
  void sortByName();
  bool applyTheme(size_t index, const std::string& cachePath = {});
  bool canRevertTheme(const std::string& cachePath) const;
  bool revertThemeChange(const std::string& cachePath);
  static bool loadBookSettings(const std::string& cachePath,
                               ReadingTheme& theme);
  static bool saveCurrentBookSettings(const std::string& cachePath);
  static bool loadBookSettingsIntoCurrent(const std::string& cachePath);

  static ReadingTheme fromSettings(const std::string& name,
                                   const CrossPointSettings& settings);
  static ReadingTheme normalizeTheme(const ReadingTheme& theme);
  static void applyThemeToSettings(const ReadingTheme& theme,
                                   CrossPointSettings& settings);
  static std::string sanitizeName(const std::string& name);
};

#define READING_THEMES ReadingThemeStore::getInstance()
