/**
 * @file CrossPointSettings.h
 * @brief Device settings model — enums, defaults, and persistence interface.
 *
 * Defines all user-configurable settings (fonts, margins, themes, network,
 * button remapping, etc.) as a flat struct. Settings are serialized to JSON
 * on the SD card at /.crosspoint/settings.json via JsonSettingsIO.
 *
 * Legacy binary format (pre-DX34) is still readable for migration; see
 * loadFromBinaryFile(). Enum values marked LEGACY_* are kept so that
 * normalizeFontFamily() can upgrade old settings files without data loss.
 */
#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <iosfwd>
#include <string>

class CrossPointSettings {
 public:
  // Default-constructible + copy-assignable so PersistentStore<T> can hold
  // and reload the value. Activities still touch the canonical instance
  // via getInstance(); the only legitimate copy/assign sites are inside
  // PersistentStore::load() (parsed -> data_) and tests.
  CrossPointSettings() = default;
  CrossPointSettings(const CrossPointSettings&) = default;
  CrossPointSettings& operator=(const CrossPointSettings&) = default;

  enum SLEEP_SCREEN_MODE {
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    BLANK = 4,
    COVER_CUSTOM = 5,
    QUOTES = 6,
    QUOTES_CUSTOM = 7,
    FREEZE = 8,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };
  // Status bar display type enum
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_TEXT_ALIGNMENT {
    STATUS_TEXT_RIGHT = 0,
    STATUS_TEXT_CENTER = 1,
    STATUS_TEXT_LEFT = 2,
    STATUS_TEXT_ALIGNMENT_COUNT
  };
  enum STATUS_BAR_PROGRESS_STYLE {
    STATUS_BAR_THIN = 0,
    STATUS_BAR_THICK = 1,
    STATUS_BAR_DOTTED = 2,
    STATUS_BAR_PROGRESS_STYLE_COUNT
  };
  enum STATUS_BAR_PAGE_COUNTER_MODE {
    STATUS_PAGE_CURRENT_OVER_TOTAL = 0,
    STATUS_PAGE_LEFT_TEXT = 1,
    STATUS_BAR_PAGE_COUNTER_MODE_COUNT
  };
  enum STATUS_BAR_FONT_SIZE { STATUS_FONT_SMALL = 0, STATUS_FONT_MEDIUM = 1, STATUS_BAR_FONT_SIZE_COUNT };
  enum STATUS_BAR_BAR_THICKNESS {
    STATUS_BAR_THICKNESS_NORMAL = 0,
    STATUS_BAR_THICKNESS_DOUBLE = 1,
    STATUS_BAR_BAR_THICKNESS_COUNT
  };
  // Home screen layout modes
  enum HOME_LAYOUT { HOME_LAYOUT_CLASSIC = 0, HOME_LAYOUT_SINGLE_COVER = 1, HOME_LAYOUT_COUNT };

  enum STATUS_BAR_ITEM_POSITION { STATUS_AT_TOP = 0, STATUS_AT_BOTTOM = 1, STATUS_BAR_ITEM_POSITION_COUNT };
  enum STATUS_BAR_TEXT_POSITION {
    STATUS_TEXT_TOP_LEFT = 0,
    STATUS_TEXT_TOP_CENTER = 1,
    STATUS_TEXT_TOP_RIGHT = 2,
    STATUS_TEXT_BOTTOM_LEFT = 3,
    STATUS_TEXT_BOTTOM_CENTER = 4,
    STATUS_TEXT_BOTTOM_RIGHT = 5,
    STATUS_BAR_TEXT_POSITION_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Previous, Next
  // Swapped: Next, Previous
  enum SIDE_BUTTON_LAYOUT { PREV_NEXT = 0, NEXT_PREV = 1, SIDE_BUTTON_LAYOUT_COUNT };

  // Font family options. Values 3-8 are legacy (removed families) and
  // normalize to CHAREINK via normalizeFontFamily(). GALMURI uses value 9
  // (first slot after the legacy-removed range).
  enum FONT_FAMILY {
    CHAREINK = 0,
    BOOKERLY = 1,
    VOLLKORN = 2,
    GALMURI = 9,
    // TT2020 (was 10) removed in 2026-04 to reclaim flash. Legacy value 10
    // normalizes to CHAREINK via normalizeFontFamily().
    BITTER = 11,
    // Phase 2b: single custom-font slot bound to unifont_16. When no custom
    // font is registered at runtime the reader falls back to CHAREINK.
    // Named CUSTOM_FAMILY (not CUSTOM) because the SLEEP_SCREEN_MODE enum
    // in this same class already uses CUSTOM — C++ unscoped enums share
    // a single name space per containing class.
    CUSTOM_FAMILY = 12,
    FONT_FAMILY_COUNT
  };
  enum FONT_SIZE {
    MEDIUM = 0,   // legacy 15pt -> normalize to SIZE_14
    LARGE = 1,    // 17pt
    X_LARGE = 2,  // legacy 19 -> normalize to LARGE
    SIZE_14 = 3,
    SIZE_16 = 4,
    SIZE_18 = 5,  // legacy -> normalize to LARGE (17)
    SIZE_13 = 6,
    SIZE_12 = 7,
    // SIZE_10 was previously a legacy-only value (normalized to SIZE_12). With
    // the experimental Galmuri pixel font it becomes a real selectable size
    // when fontFamily == GALMURI. For all other families it still normalizes
    // to SIZE_12.
    SIZE_10 = 8,
    SIZE_15 = 9,
    SIZE_11 = 10,  // Galmuri-only selectable size; normalizes to SIZE_12 for others.
    FONT_SIZE_COUNT
  };
  // Legacy line spacing enum (kept for settings migration compatibility)
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };
  enum FIRST_LINE_INDENT_MODE {
    INDENT_BOOK = 0,
    INDENT_OFF = 1,
    INDENT_SMALL = 2,
    INDENT_MEDIUM = 3,
    INDENT_LARGE = 4,
    FIRST_LINE_INDENT_MODE_COUNT
  };
  enum READER_STYLE_MODE { READER_STYLE_USER = 0, READER_STYLE_HYBRID = 1, READER_STYLE_MODE_COUNT };
  enum TEXT_RENDER_MODE { TEXT_RENDER_CRISP = 0, TEXT_RENDER_DARK = 1, TEXT_RENDER_BIONIC = 2, TEXT_RENDER_MODE_COUNT };
  enum EXTRA_PARAGRAPH_SPACING_LEVEL {
    EXTRA_SPACING_OFF = 0,
    EXTRA_SPACING_S = 1,
    EXTRA_SPACING_M = 2,
    EXTRA_SPACING_L = 3,
    EXTRA_SPACING_XL = 4,
    EXTRA_SPACING_XXL = 5,
    EXTRA_PARAGRAPH_SPACING_COUNT
  };

  enum WORD_SPACING_MODE {
    WORD_SPACING_TIGHT = 0,
    WORD_SPACING_NORMAL = 1,
    WORD_SPACING_WIDE = 2,
    WORD_SPACING_VERY_WIDE = 3,
    WORD_SPACING_EXTRA_WIDE = 4,
    WORD_SPACING_MODE_COUNT
  };

  enum HIGHLIGHT_MODE { HIGHLIGHT_WORD = 0, HIGHLIGHT_PAGE = 1, HIGHLIGHT_MODE_COUNT };

  // Image dithering algorithm
  enum IMAGE_DITHER {
    IMAGE_DITHER_FAST = 0,     // Bayer ordered dither (default, fast)
    IMAGE_DITHER_QUALITY = 1,  // Floyd-Steinberg error diffusion (better photos)
    IMAGE_DITHER_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_NEVER = 5,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, SHORT_PWRBTN_COUNT };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Home screen layout
  uint8_t homeLayout = HOME_LAYOUT_CLASSIC;
  // Sleep screen settings
  uint8_t sleepScreen = DARK;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Show custom sleep image filename label
  uint8_t showSleepImageFilename = 0;
  // Status bar settings
  uint8_t statusBar = FULL;
  uint8_t statusBarEnabled = 1;
  uint8_t statusBarShowBattery = 1;
  uint8_t statusBarShowPageCounter = 0;
  uint8_t statusBarPageCounterMode = STATUS_PAGE_CURRENT_OVER_TOTAL;
  uint8_t statusBarShowBookPercentage = 0;
  uint8_t statusBarShowChapterPercentage = 0;
  uint8_t statusBarShowBookBar = 0;
  uint8_t statusBarShowChapterBar = 0;
  uint8_t statusBarShowChapterTitle = 1;
  uint8_t statusBarNoTitleTruncation = 0;
  uint8_t statusBarTopLine = 0;
  uint8_t statusBarBatteryPosition = STATUS_TEXT_BOTTOM_LEFT;
  uint8_t statusBarProgressTextPosition = STATUS_AT_BOTTOM;
  uint8_t statusBarPageCounterPosition = STATUS_TEXT_BOTTOM_CENTER;
  uint8_t statusBarBookPercentagePosition = STATUS_TEXT_BOTTOM_CENTER;
  uint8_t statusBarChapterPercentagePosition = STATUS_TEXT_BOTTOM_CENTER;
  uint8_t statusBarBookBarPosition = STATUS_AT_BOTTOM;
  uint8_t statusBarChapterBarPosition = STATUS_AT_BOTTOM;
  uint8_t statusBarTitlePosition = STATUS_AT_BOTTOM;
  uint8_t statusBarTextAlignment = STATUS_TEXT_RIGHT;
  uint8_t statusBarProgressStyle = STATUS_BAR_THICK;
  // Retained for JSON back-compat only; no longer user-configurable. The status
  // bar always uses the larger font (see getStatusBarFontId()). Default now MEDIUM.
  uint8_t statusBarFontSize = STATUS_FONT_MEDIUM;
  uint8_t statusBarBarThickness = STATUS_BAR_THICKNESS_NORMAL;
  uint8_t statusBarShowBookPageCounter = 0;
  uint8_t statusBarBookPageCounterPosition = STATUS_TEXT_BOTTOM_CENTER;
  // Text rendering settings
  uint8_t extraParagraphSpacingLevel = EXTRA_SPACING_M;
  // Legacy field name retained for storage compatibility; value stores a
  // word-spacing level (0..4, see WORD_SPACING_MODE enum) instead of a raw percentage.
  uint8_t wordSpacingPercent = WORD_SPACING_NORMAL;
  uint8_t firstLineIndentMode = INDENT_BOOK;
  uint8_t readerStyleMode = READER_STYLE_USER;
  uint8_t textRenderMode = TEXT_RENDER_CRISP;
  // Legacy binary-compat field; always 0. Do not remove (breaks serialization).
  uint8_t textAntiAliasing = 0;
  // Factory LUT grayscale (off-by-default; ships behind a settings toggle for first release).
  // When 1: image-bearing pages, BMP viewer, and sleep covers render via the panel's factory
  // waveform — sharper grayscale at the cost of a black flash before each refresh. When 0:
  // current differential grayscale path. Also gates the dither/brightness retune in
  // BitmapHelpers tuned for the factory LUT's response curve.
  uint8_t useFactoryLUT = 1;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 =
  // landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front
  // buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = CHAREINK;
  uint8_t fontSize = SIZE_16;
  // When fontFamily == CUSTOM_FAMILY, this names the user-dropped family
  // to dispatch to (filename stem, e.g. "unifont"). Empty string falls
  // back to CHAREINK at reader-font-id resolution time.
  std::string customFontName;
  // Selected pixel size for the active custom family. Paired with
  // customFontName so a family dropped at multiple sizes (e.g.
  // fontA_17.bdf + fontA_20.bdf) can be switched via the reader Font
  // Size picker. 0 = "not yet picked"; picker first-opens defaults to
  // the smallest available size.
  uint8_t customFontSizePt = 0;
  // Legacy line spacing setting (kept for migration from old settings files)
  uint8_t lineSpacing = NORMAL;
  // Reader line spacing percentage (35..150)
  uint8_t lineSpacingPercent = 110;
  uint8_t paragraphAlignment = JUSTIFIED;
  // Auto-sleep timeout setting (default 10 minutes)
  uint8_t sleepTimeout = SLEEP_10_MIN;
  // Show hidden files/directories (starting with '.') in file browser
  uint8_t showHiddenFiles = 0;
  // Open a random epub from recents on boot instead of the last book
  uint8_t randomBookOnBoot = 0;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;
  // UI language. Stored as the uint8_t value of i18n `Language` enum
  // (0 = ENGLISH). Validated against getLanguageCount() on load.
  uint8_t uiLanguage = 0;

  // Legacy uniform reader margin (kept for backward compatibility in settings
  // migration)
  uint8_t screenMargin = 20;
  // Reader screen margin settings
  uint8_t uniformMargins = 0;  // 0 = separate margins, 1 = uniform (all sides equal)
  uint8_t dynamicMargins = 0;  // 0 = off, 1 = auto-calculate (10px min), 2 = auto-calculate (20px min)
  uint8_t screenMarginHorizontal = 20;
  uint8_t screenMarginTop = 20;
  uint8_t screenMarginBottom = 20;
  // OPDS browser settings
  char opdsServerUrl[128] = "";
  char opdsUsername[64] = "";
  char opdsPassword[64] = "";
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // Long-press chapter skip on side buttons
  uint8_t longPressChapterSkip = 1;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Legacy compatibility field migrated into readerStyleMode.
  uint8_t embeddedStyle = 1;
  // Draw dotted debug borders around reader and status bar viewports
  uint8_t debugBorders = 0;
  // Highlight/quote selection method: HIGHLIGHT_WORD (pick start/end) or HIGHLIGHT_PAGE (full page narrow-down)
  uint8_t highlightMode = HIGHLIGHT_WORD;
  // Dark mode: invert display output (white-on-black)
  uint8_t darkMode = 0;
  // Books folder display order: 0 = alphabetical (default), 1 = random
  uint8_t booksFolderOrder = 0;
  // Image dithering algorithm: 0=fast (Bayer), 1=quality (Floyd-Steinberg)
  uint8_t imageDither = IMAGE_DITHER_FAST;

  ~CrossPointSettings() = default;

  // Get singleton instance. Backed by PersistentStore<CrossPointSettings>'s
  // owned data — defined in CrossPointSettings.cpp so the header does not
  // pull SettingsStore + PersistentStore + IFileIO into every translation
  // unit that includes settings.
  static CrossPointSettings& getInstance();

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? 10 : 700;
  }
  static uint8_t normalizeFontFamily(uint8_t family);
  static uint8_t fontFamilyToDisplayIndex(uint8_t family);
  static uint8_t displayIndexToFontFamily(uint8_t displayIndex);
  static uint8_t normalizeFontSizeForFamily(uint8_t family, uint8_t fontSize);
  static uint8_t resetLineSpacingPercentForFamily(uint8_t family);
  static uint8_t nextFontSize(uint8_t family, uint8_t fontSize);
  static uint8_t fontSizeToPointSize(uint8_t family, uint8_t fontSize);
  static uint8_t fontSizeOptionCount(uint8_t family);
  static uint8_t fontSizeToDisplayIndex(uint8_t family, uint8_t fontSize);
  static uint8_t displayIndexToFontSize(uint8_t family, uint8_t displayIndex);
  static uint8_t normalizeStatusBarPageCounterMode(uint8_t mode);
  static int wordSpacingSettingToPixelDelta(uint8_t mode, int baseSpaceWidth);
  int getReaderFontId() const;
  int getStatusBarProgressBarHeight() const;
  int getStatusBarFontId() const;

  bool saveToFile() const;
  bool loadFromFile();

  static void validateFrontButtonMapping(CrossPointSettings& settings);

 private:
  bool loadFromBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()

// Migration helper — converts legacy statusBar enum into individual flags.
// Defined in CrossPointSettings.cpp, also used by JsonSettingsIO.cpp.
void migrateLegacyStatusBarMode(CrossPointSettings& settings);
