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
  enum STATUS_BAR_BAR_THICKNESS {
    STATUS_BAR_THICKNESS_NORMAL = 0,
    STATUS_BAR_THICKNESS_DOUBLE = 1,
    STATUS_BAR_BAR_THICKNESS_COUNT
  };
  // Home screen layout modes
  enum HOME_LAYOUT { HOME_LAYOUT_CLASSIC = 0, HOME_LAYOUT_SINGLE_COVER = 1, HOME_LAYOUT_COUNT };
  // Visual style of the quotes viewer + in-book quote-selection frame
  enum QUOTE_SCREEN_STYLE { QUOTE_STYLE_CLASSIC = 0, QUOTE_STYLE_TERMINAL = 1, QUOTE_STYLE_COUNT };

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
  // What the status-bar title row renders: the current chapter title (default)
  // or the book title + author. EPUB-only; TXT always shows the file name.
  enum STATUS_BAR_TITLE_CONTENT {
    STATUS_TITLE_CHAPTER = 0,
    STATUS_TITLE_BOOK_AUTHOR = 1,
    STATUS_BAR_TITLE_CONTENT_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // X3-only gyro tilt page-turn. Values MUST match HalTiltSensor's
  // CrossPointTiltPageTurn (TILT_OFF=0, TILT_NORMAL=1, TILT_INVERTED=2).
  enum TILT_PAGE_TURN { TILT_OFF = 0, TILT_NORMAL = 1, TILT_INVERTED = 2, TILT_PAGE_TURN_COUNT };

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

  // Font family options. Several values are legacy gaps left intentionally so a
  // persisted selection on a removed family normalizes to CHAREINK via
  // normalizeFontFamily() instead of aliasing to a different family:
  //   2    VOLLKORN (removed 2026-06)
  //   3-8  legacy removed families
  //   9    GALMURI  (removed 2026-06)
  //   10   TT2020   (removed 2026-04)
  //   11   BITTER   (removed 2026-06)
  //   12   CUSTOM   (user-uploaded .bin custom-font feature removed 2026-06)
  //   13   F25_BANK_PRINTER  (removed 2026-06)
  //   15   PIXEL32           (removed 2026-06)
  //   22   PIXELOPERATOR     (brief "for fun" reader font; removed 2026-06. Pixel
  //        Operator is still the UI font — that is separate from this reader enum.)
  enum FONT_FAMILY {
    CHAREINK = 0,
    BOOKERLY = 1,
    GEORGIA = 14,  // serif reader font (sizes 10, 12, 14, 16, 17)
    // 16 (ETBB / ET Book) and 17 (ROSARIVO) removed — kept as enum gaps so a
    // persisted value normalizes to CHAREINK rather than re-mapping to another
    // font. Do not reuse for a new family.
    LATO = 18,  // humanist sans-serif reader font (sizes 10, 12, 14, 16, 17)
    // 19 (COZETTE) was a brief reader font; removed. Kept as an enum gap so a
    // persisted value normalizes to CHAREINK. Cozette is no longer bundled at all.
    HELVETICA = 20,  // grotesque sans-serif reader font (sizes 10, 12, 14, 16, 17)
    VERDANA = 21,    // humanist sans-serif reader font (sizes 10, 12, 14, 16, 17)
    // 22 (PIXELOPERATOR) removed — kept as an enum gap so a persisted value
    // normalizes to CHAREINK. (Pixel Operator remains the UI font; unrelated.)
    MERRIWEATHER = 23,  // serif reader font (SD-only; sizes 10..18, all 4 weights)
    PLAYFAIR = 24,      // high-contrast serif reader font (SD-only; sizes 10..18)
    GALMURI = 25,       // pixel font (SD-only; native crisp 14px/28px, mapped onto 10..18).
                        // NOTE distinct from the old removed GALMURI=9 gap (do not reuse 9).
    VOLLKORN = 26,      // serif reader font (SD-only; sizes 10..18, all 4 weights, real kerning).
                        // NOTE distinct from the old removed VOLLKORN=2 gap (do not reuse 2).
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
    INDENT_MEGA = 5,  // first line starts ~1/3 of the way across the column
    FIRST_LINE_INDENT_MODE_COUNT
  };
  enum READER_STYLE_MODE { READER_STYLE_USER = 0, READER_STYLE_HYBRID = 1, READER_STYLE_MODE_COUNT };
  // User-facing text render: two options only — Normal (plain render, no weight
  // effect) and Dark (a heavier pass). The numeric values ARE the picker row order
  // (generic ENUM settings store the option index). Older files used a 5-way
  // weight-order palette (Thin0 Crisp1 Medium2 Dark3 Bionic4), and even older ones
  // a 4-way order; both are collapsed to Normal/Dark on load in JsonSettingsIO
  // (gated by the textRenderModeNormalDark flag).
  enum TEXT_RENDER_MODE {
    TEXT_RENDER_NORMAL = 0,  // plain 1-bit blit, no weight effect (engine "crisp" style)
    TEXT_RENDER_DARK = 1,    // heavier/darker pass (engine "dark" style)
    TEXT_RENDER_MODE_COUNT
  };
  // Engine text-render-STYLE palette fed to GfxRenderer::setTextRenderStyle. NOT the
  // user setting — these are the renderer's historical weight-order blit styles, of
  // which only Crisp + Dark are now reachable (via renderStyleForTextMode). Kept as
  // named constants so the status bar + OOM recovery can force plain (crisp) text,
  // and so old persisted values migrate through the same numbering.
  static constexpr uint8_t kRenderStyleCrisp = 1;
  static constexpr uint8_t kRenderStyleDark = 3;
  // Translate a user render mode to the engine render style passed to the renderer.
  static constexpr uint8_t renderStyleForTextMode(const uint8_t mode) {
    return mode == TEXT_RENDER_DARK ? kRenderStyleDark : kRenderStyleCrisp;
  }
  // Maps a pre-weight-order render-mode value (Crisp=0,Dark=1,Bionic=2,Thin=3) to
  // the weight-order STYLE palette. Pure + header-inline. Unknown input -> Crisp.
  static constexpr uint8_t migrateTextRenderModeToWeightOrder(const uint8_t legacy) {
    // Legacy v2 numbering was Crisp=0, Dark=1, Bionic=2, Thin=3. Lector keeps only
    // Crisp/Dark, so old Dark collapses to dark; everything else to crisp.
    return legacy == 1 ? kRenderStyleDark : kRenderStyleCrisp;
  }
  // Collapse a weight-order STYLE value (0..4) to the 2-way user mode: only the
  // Dark style maps to Dark; every other style is Normal.
  static constexpr uint8_t collapseRenderStyleToMode(const uint8_t style) {
    return style == kRenderStyleDark ? TEXT_RENDER_DARK : TEXT_RENDER_NORMAL;
  }
  enum EXTRA_PARAGRAPH_SPACING_LEVEL {
    EXTRA_SPACING_OFF = 0,
    EXTRA_SPACING_S = 1,
    EXTRA_SPACING_M = 2,
    EXTRA_SPACING_L = 3,
    EXTRA_SPACING_XL = 4,
    EXTRA_SPACING_XXL = 5,
    EXTRA_SPACING_XXXL = 6,  // ~80% of line height (appended; existing 0..5 unchanged)
    EXTRA_PARAGRAPH_SPACING_COUNT
  };

  // Visual order (the picker stores the index == enum value). Midpoints inserted
  // between the original five, plus a new top step, so persisted pre-insert values
  // are remapped on load (migrateWordSpacingToMidpoints + JsonSettingsIO flag).
  enum WORD_SPACING_MODE {
    WORD_SPACING_TIGHT = 0,       // -30%
    WORD_SPACING_NORMAL = 1,      // 0%
    WORD_SPACING_P40 = 2,         // +40%  (inserted)
    WORD_SPACING_WIDE = 3,        // +80%
    WORD_SPACING_P115 = 4,        // +115% (inserted)
    WORD_SPACING_VERY_WIDE = 5,   // +150%
    WORD_SPACING_P195 = 6,        // +195% (inserted)
    WORD_SPACING_EXTRA_WIDE = 7,  // +240%
    WORD_SPACING_P300 = 8,        // +300% (inserted, after 240)
    WORD_SPACING_MODE_COUNT
  };
  // Remap a pre-midpoint persisted word-spacing value (TIGHT0 NORMAL1 WIDE2
  // VERY_WIDE3 EXTRA_WIDE4) to the current visual-order enum. Identity for the
  // two lowest (0,1); the three wide steps shift up past the inserted midpoints.
  static constexpr uint8_t migrateWordSpacingToMidpoints(const uint8_t legacy) {
    switch (legacy) {
      case 2:
        return WORD_SPACING_WIDE;  // old +80%  (2) -> 3
      case 3:
        return WORD_SPACING_VERY_WIDE;  // old +150% (3) -> 5
      case 4:
        return WORD_SPACING_EXTRA_WIDE;  // old +240% (4) -> 7
      default:
        return legacy <= WORD_SPACING_NORMAL ? legacy : WORD_SPACING_NORMAL;
    }
  }

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
    SLEEP_NEVER = 5,
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
  // Show a small "[F]" badge bottom-left when the sleep wallpaper is a favorite
  uint8_t showSleepFavoriteBadge = 0;
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
  uint8_t statusBarBarThickness = STATUS_BAR_THICKNESS_NORMAL;
  // Pages remaining to the end of the current chapter (whole file for TXT).
  uint8_t statusBarShowPagesLeft = 0;
  uint8_t statusBarPagesLeftPosition = STATUS_TEXT_BOTTOM_RIGHT;
  // Title row content: chapter title vs book title + author (EPUB only).
  uint8_t statusBarTitleContent = STATUS_TITLE_CHAPTER;
  // "Ch N/M" chapter-index readout (EPUB only; blank for TXT).
  uint8_t statusBarShowChapterNumber = 0;
  uint8_t statusBarChapterNumberPosition = STATUS_TEXT_BOTTOM_LEFT;
  // "N quotes" saved-quote counter for the current book (EPUB only).
  uint8_t statusBarShowQuoteCount = 0;
  uint8_t statusBarQuoteCountPosition = STATUS_TEXT_BOTTOM_RIGHT;
  // "RAM NNNK" free-heap debug readout.
  uint8_t statusBarShowFreeHeap = 0;
  uint8_t statusBarFreeHeapPosition = STATUS_TEXT_TOP_RIGHT;
  // Visual style of the quotes viewer (Classic / Terminal / Index-card / Manuscript)
  uint8_t quoteScreenStyle = QUOTE_STYLE_CLASSIC;
  // Text rendering settings
  uint8_t extraParagraphSpacingLevel = EXTRA_SPACING_M;
  // Legacy field name retained for storage compatibility; value stores a
  // word-spacing level (0..4, see WORD_SPACING_MODE enum) instead of a raw percentage.
  uint8_t wordSpacingPercent = WORD_SPACING_NORMAL;
  uint8_t firstLineIndentMode = INDENT_BOOK;
  uint8_t readerStyleMode = READER_STYLE_USER;
  uint8_t textRenderMode = TEXT_RENDER_NORMAL;
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
  // X3-only gyro tilt page-turn (TILT_OFF default; no-op on X4). Gated in the
  // settings UI on halTiltSensor.isAvailable().
  uint8_t tiltPageTurn = TILT_OFF;
  // X3-only DS3231 clock UI (all gated on halClock.isAvailable(), so inert on X4).
  uint8_t statusBarClock = 0;      // show clock in the reader status bar
  uint8_t clockUtcOffsetQ = 48;    // biased quarter-hour UTC offset (48 = UTC+0)
  uint8_t clockFormat = 0;         // 0 = 24-hour, 1 = 12-hour AM/PM
  uint8_t clockHasBeenSynced = 0;  // set once after the first successful NTP sync
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
  uint8_t fontFamily = BOOKERLY;  // Lector: Bookerly is the default reader font (ChareInk removed).
  uint8_t fontSize = SIZE_16;
  // Transient (NEVER serialized) emergency render-degrade latch. When the
  // reader exhausts the heap mid-render on a fragmented heap, it sets this so
  // getReaderFontId() resolves to the smallest built-in font (CHAREINK 12),
  // whose tiny glyph groups fit the largest free block. Because JsonSettingsIO
  // never writes it, it physically cannot leak to disk — the user's real font
  // is restored on the next book open (the reader also clears it on exit). See
  // EpubReaderActivity render-OOM recovery.
  bool emergencyRenderFontDowngrade = false;
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
  // Smooth text (anti-aliasing): 0 = off (crisp 1-bit, fast). 1 = render glyph
  // grey edges via a greyscale overlay → smoother text, but every page takes the
  // slow greyscale refresh (~3x slower turns). Global, off by default (Snappy LAW).
  uint8_t smoothText = 0;
  // Reader screen margin settings
  uint8_t uniformMargins = 0;  // 0 = separate margins, 1 = uniform (all sides equal)
  uint8_t dynamicMargins = 0;  // 0 = off, 1 = auto-calculate (10px min), 2 = auto-calculate (20px min)
  uint8_t screenMarginHorizontal = 20;
  uint8_t screenMarginTop = 20;
  uint8_t screenMarginBottom = 20;
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
  uint8_t imageDither = IMAGE_DITHER_QUALITY;

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
  // Internal helper: true when the family gains the SD-only in-between/large
  // sizes (10..18). Defined in CrossPointSettingsLogic.cpp.
  static bool familyHasExtraSizes(uint8_t family);
  bool loadFromBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

// --- Compile-time contract for the weight-ordered render-mode enum ---
// Generic ENUM settings store the picker row index as the on-disk value, so the
// enum's numeric order IS the picker order AND the persisted value. Pin both the
// order and the legacy->weight-order migration map so a future renumber can't
// silently desync the picker, the renderer literals (GfxRenderer.cpp), or the
// JsonSettingsIO migration.
static_assert(CrossPointSettings::kRenderStyleCrisp == 1, "render-style weight order");
static_assert(CrossPointSettings::kRenderStyleDark == 3, "render-style weight order");
static_assert(CrossPointSettings::TEXT_RENDER_NORMAL == 0, "user render mode order");
static_assert(CrossPointSettings::TEXT_RENDER_DARK == 1, "user render mode order");
static_assert(CrossPointSettings::TEXT_RENDER_MODE_COUNT == 2, "render-mode count");
// renderStyleForTextMode: Normal -> crisp style, Dark -> dark style.
static_assert(CrossPointSettings::renderStyleForTextMode(CrossPointSettings::TEXT_RENDER_NORMAL) ==
                  CrossPointSettings::kRenderStyleCrisp,
              "");
static_assert(CrossPointSettings::renderStyleForTextMode(CrossPointSettings::TEXT_RENDER_DARK) ==
                  CrossPointSettings::kRenderStyleDark,
              "");
// Legacy (Crisp=0,Dark=1,Bionic=2,Thin=3) -> weight-order STYLE palette.
static_assert(CrossPointSettings::migrateTextRenderModeToWeightOrder(0) == CrossPointSettings::kRenderStyleCrisp, "");
static_assert(CrossPointSettings::migrateTextRenderModeToWeightOrder(1) == CrossPointSettings::kRenderStyleDark, "");
static_assert(CrossPointSettings::migrateTextRenderModeToWeightOrder(99) == CrossPointSettings::kRenderStyleCrisp,
              "unknown legacy render mode -> crisp");
// Weight-order STYLE -> 2-way user mode collapse (only Dark stays Dark).
static_assert(CrossPointSettings::collapseRenderStyleToMode(CrossPointSettings::kRenderStyleDark) ==
                  CrossPointSettings::TEXT_RENDER_DARK,
              "");
static_assert(CrossPointSettings::collapseRenderStyleToMode(CrossPointSettings::kRenderStyleCrisp) ==
                  CrossPointSettings::TEXT_RENDER_NORMAL,
              "");

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()

// Migration helper — converts legacy statusBar enum into individual flags.
// Defined in CrossPointSettings.cpp, also used by JsonSettingsIO.cpp.
void migrateLegacyStatusBarMode(CrossPointSettings& settings);
