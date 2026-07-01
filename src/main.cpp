/**
 * @file main.cpp
 * @brief Application entry point, activity manager, and power management.
 *
 * Initializes hardware (display, SD card, fonts, GPIO), loads persisted
 * settings/state, and runs the main loop. The loop dispatches to the
 * current Activity (screen), polls buttons, manages sleep timeouts,
 * and handles deep-sleep entry/exit via RTC wakeup.
 *
 * Activity transitions are callback-driven: each Activity calls
 * onGoHome(), onGoToReader(), etc. which swap the global currentActivity.
 */
#include <Arduino.h>
#include <BitmapHelpers.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryPolicy.h>
#include <SPI.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <new>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "Paths.h"
#include "ReadingThemeStore.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "boot/BootSequenceOrchestrator.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#ifdef CROSSPOINT_SD_FONTS
#include "fonts/HalSdFontIo.h"
#include "fonts/ReaderFontActivation.h"
#include "fonts/SdFontManager.h"
#endif  // CROSSPOINT_SD_FONTS

// The 5 flash reader families' in-between sizes 11/13/15. Baked into flash in the
// flash-extra build (default) so the size picker offers {11,12,13,14,15,16,17};
// also present in SD builds, where the bitmaps stream from /fonts/*.bin packs (and
// slim drops them via offload_bitmaps.py). Headers are NOT in all.h. Size 18 of
// these families + the SD-only families (Merriweather/Playfair/Galmuri/Vollkorn)
// stay SD-only — they are never baked into the flash build (gated below).
#if defined(CROSSPOINT_SD_FONTS) || defined(CROSSPOINT_FLASH_EXTRA_SIZES)
#include <builtinFonts/bookerly_11_bold.h>
#include <builtinFonts/bookerly_11_italic.h>
#include <builtinFonts/bookerly_11_regular.h>
#include <builtinFonts/bookerly_13_bold.h>
#include <builtinFonts/bookerly_13_italic.h>
#include <builtinFonts/bookerly_13_regular.h>
#include <builtinFonts/bookerly_15_bold.h>
#include <builtinFonts/bookerly_15_italic.h>
#include <builtinFonts/bookerly_15_regular.h>
#include <builtinFonts/georgia_11_bold.h>
#include <builtinFonts/georgia_11_italic.h>
#include <builtinFonts/georgia_11_regular.h>
#include <builtinFonts/georgia_13_bold.h>
#include <builtinFonts/georgia_13_italic.h>
#include <builtinFonts/georgia_13_regular.h>
#include <builtinFonts/georgia_15_bold.h>
#include <builtinFonts/georgia_15_italic.h>
#include <builtinFonts/georgia_15_regular.h>
#include <builtinFonts/helvetica_11_bold.h>
#include <builtinFonts/helvetica_11_italic.h>
#include <builtinFonts/helvetica_11_regular.h>
#include <builtinFonts/helvetica_13_bold.h>
#include <builtinFonts/helvetica_13_italic.h>
#include <builtinFonts/helvetica_13_regular.h>
#include <builtinFonts/helvetica_15_bold.h>
#include <builtinFonts/helvetica_15_italic.h>
#include <builtinFonts/helvetica_15_regular.h>
#include <builtinFonts/lato_11_bold.h>
#include <builtinFonts/lato_11_bolditalic.h>
#include <builtinFonts/lato_11_italic.h>
#include <builtinFonts/lato_11_regular.h>
#include <builtinFonts/lato_13_bold.h>
#include <builtinFonts/lato_13_bolditalic.h>
#include <builtinFonts/lato_13_italic.h>
#include <builtinFonts/lato_13_regular.h>
#include <builtinFonts/lato_15_bold.h>
#include <builtinFonts/lato_15_bolditalic.h>
#include <builtinFonts/lato_15_italic.h>
#include <builtinFonts/lato_15_regular.h>
#include <builtinFonts/verdana_11_bold.h>
#include <builtinFonts/verdana_11_italic.h>
#include <builtinFonts/verdana_11_regular.h>
#include <builtinFonts/verdana_13_bold.h>
#include <builtinFonts/verdana_13_italic.h>
#include <builtinFonts/verdana_13_regular.h>
#include <builtinFonts/verdana_15_bold.h>
#include <builtinFonts/verdana_15_italic.h>
#include <builtinFonts/verdana_15_regular.h>
#endif  // flash extra sizes 11/13/15

#ifdef CROSSPOINT_SD_FONTS
// SD-only Tier-1: size 18 of the 5 flash families + (after these) the SD-only
// reader families. Never baked into the flash build — the picker tops out at 17.
#include <builtinFonts/bookerly_18_bold.h>
#include <builtinFonts/bookerly_18_italic.h>
#include <builtinFonts/bookerly_18_regular.h>
#include <builtinFonts/galmuri_14_bold.h>
#include <builtinFonts/galmuri_14_italic.h>
#include <builtinFonts/galmuri_14_regular.h>
#include <builtinFonts/galmuri_28_bold.h>
#include <builtinFonts/galmuri_28_italic.h>
#include <builtinFonts/galmuri_28_regular.h>
#include <builtinFonts/georgia_18_bold.h>
#include <builtinFonts/georgia_18_italic.h>
#include <builtinFonts/georgia_18_regular.h>
#include <builtinFonts/helvetica_18_bold.h>
#include <builtinFonts/helvetica_18_italic.h>
#include <builtinFonts/helvetica_18_regular.h>
#include <builtinFonts/lato_18_bold.h>
#include <builtinFonts/lato_18_bolditalic.h>
#include <builtinFonts/lato_18_italic.h>
#include <builtinFonts/lato_18_regular.h>
#include <builtinFonts/merriweather_10_bold.h>
#include <builtinFonts/merriweather_10_bolditalic.h>
#include <builtinFonts/merriweather_10_italic.h>
#include <builtinFonts/merriweather_10_regular.h>
#include <builtinFonts/merriweather_11_bold.h>
#include <builtinFonts/merriweather_11_bolditalic.h>
#include <builtinFonts/merriweather_11_italic.h>
#include <builtinFonts/merriweather_11_regular.h>
#include <builtinFonts/merriweather_12_bold.h>
#include <builtinFonts/merriweather_12_bolditalic.h>
#include <builtinFonts/merriweather_12_italic.h>
#include <builtinFonts/merriweather_12_regular.h>
#include <builtinFonts/merriweather_13_bold.h>
#include <builtinFonts/merriweather_13_bolditalic.h>
#include <builtinFonts/merriweather_13_italic.h>
#include <builtinFonts/merriweather_13_regular.h>
#include <builtinFonts/merriweather_14_bold.h>
#include <builtinFonts/merriweather_14_bolditalic.h>
#include <builtinFonts/merriweather_14_italic.h>
#include <builtinFonts/merriweather_14_regular.h>
#include <builtinFonts/merriweather_15_bold.h>
#include <builtinFonts/merriweather_15_bolditalic.h>
#include <builtinFonts/merriweather_15_italic.h>
#include <builtinFonts/merriweather_15_regular.h>
#include <builtinFonts/merriweather_16_bold.h>
#include <builtinFonts/merriweather_16_bolditalic.h>
#include <builtinFonts/merriweather_16_italic.h>
#include <builtinFonts/merriweather_16_regular.h>
#include <builtinFonts/merriweather_17_bold.h>
#include <builtinFonts/merriweather_17_bolditalic.h>
#include <builtinFonts/merriweather_17_italic.h>
#include <builtinFonts/merriweather_17_regular.h>
#include <builtinFonts/merriweather_18_bold.h>
#include <builtinFonts/merriweather_18_bolditalic.h>
#include <builtinFonts/merriweather_18_italic.h>
#include <builtinFonts/merriweather_18_regular.h>
#include <builtinFonts/playfair_10_bold.h>
#include <builtinFonts/playfair_10_bolditalic.h>
#include <builtinFonts/playfair_10_italic.h>
#include <builtinFonts/playfair_10_regular.h>
#include <builtinFonts/playfair_11_bold.h>
#include <builtinFonts/playfair_11_bolditalic.h>
#include <builtinFonts/playfair_11_italic.h>
#include <builtinFonts/playfair_11_regular.h>
#include <builtinFonts/playfair_12_bold.h>
#include <builtinFonts/playfair_12_bolditalic.h>
#include <builtinFonts/playfair_12_italic.h>
#include <builtinFonts/playfair_12_regular.h>
#include <builtinFonts/playfair_13_bold.h>
#include <builtinFonts/playfair_13_bolditalic.h>
#include <builtinFonts/playfair_13_italic.h>
#include <builtinFonts/playfair_13_regular.h>
#include <builtinFonts/playfair_14_bold.h>
#include <builtinFonts/playfair_14_bolditalic.h>
#include <builtinFonts/playfair_14_italic.h>
#include <builtinFonts/playfair_14_regular.h>
#include <builtinFonts/playfair_15_bold.h>
#include <builtinFonts/playfair_15_bolditalic.h>
#include <builtinFonts/playfair_15_italic.h>
#include <builtinFonts/playfair_15_regular.h>
#include <builtinFonts/playfair_16_bold.h>
#include <builtinFonts/playfair_16_bolditalic.h>
#include <builtinFonts/playfair_16_italic.h>
#include <builtinFonts/playfair_16_regular.h>
#include <builtinFonts/playfair_17_bold.h>
#include <builtinFonts/playfair_17_bolditalic.h>
#include <builtinFonts/playfair_17_italic.h>
#include <builtinFonts/playfair_17_regular.h>
#include <builtinFonts/playfair_18_bold.h>
#include <builtinFonts/playfair_18_bolditalic.h>
#include <builtinFonts/playfair_18_italic.h>
#include <builtinFonts/playfair_18_regular.h>
#include <builtinFonts/verdana_18_bold.h>
#include <builtinFonts/verdana_18_italic.h>
#include <builtinFonts/verdana_18_regular.h>
#include <builtinFonts/vollkorn_10_bold.h>
#include <builtinFonts/vollkorn_10_bolditalic.h>
#include <builtinFonts/vollkorn_10_italic.h>
#include <builtinFonts/vollkorn_10_regular.h>
#include <builtinFonts/vollkorn_11_bold.h>
#include <builtinFonts/vollkorn_11_bolditalic.h>
#include <builtinFonts/vollkorn_11_italic.h>
#include <builtinFonts/vollkorn_11_regular.h>
#include <builtinFonts/vollkorn_12_bold.h>
#include <builtinFonts/vollkorn_12_bolditalic.h>
#include <builtinFonts/vollkorn_12_italic.h>
#include <builtinFonts/vollkorn_12_regular.h>
#include <builtinFonts/vollkorn_13_bold.h>
#include <builtinFonts/vollkorn_13_bolditalic.h>
#include <builtinFonts/vollkorn_13_italic.h>
#include <builtinFonts/vollkorn_13_regular.h>
#include <builtinFonts/vollkorn_14_bold.h>
#include <builtinFonts/vollkorn_14_bolditalic.h>
#include <builtinFonts/vollkorn_14_italic.h>
#include <builtinFonts/vollkorn_14_regular.h>
#include <builtinFonts/vollkorn_15_bold.h>
#include <builtinFonts/vollkorn_15_bolditalic.h>
#include <builtinFonts/vollkorn_15_italic.h>
#include <builtinFonts/vollkorn_15_regular.h>
#include <builtinFonts/vollkorn_16_bold.h>
#include <builtinFonts/vollkorn_16_bolditalic.h>
#include <builtinFonts/vollkorn_16_italic.h>
#include <builtinFonts/vollkorn_16_regular.h>
#include <builtinFonts/vollkorn_17_bold.h>
#include <builtinFonts/vollkorn_17_bolditalic.h>
#include <builtinFonts/vollkorn_17_italic.h>
#include <builtinFonts/vollkorn_17_regular.h>
#include <builtinFonts/vollkorn_18_bold.h>
#include <builtinFonts/vollkorn_18_bolditalic.h>
#include <builtinFonts/vollkorn_18_italic.h>
#include <builtinFonts/vollkorn_18_regular.h>
#endif

// Lector UI font = Cozette, reproducing mom's DX34 v11.0.0 look exactly: three
// distinct sizes baked by convert-ui-only.sh with v11's default flags (dpi 150,
// bw-threshold 2). ui_8=status (~21px), ui_10=body/menus (~25px), ui_12=titles
// (~29px). Regular only — hierarchy comes from size, not weight.
#include <builtinFonts/ui_8_regular.h>
#include <builtinFonts/ui_10_regular.h>
#include <builtinFonts/ui_12_regular.h>

// Lector: Merriweather bitmaps baked into flash (SD builds pull these via the SD
// include block above; the flash build needs them explicitly for sizes 11..17).
#if defined(CROSSPOINT_FLASH_EXTRA_SIZES) && !defined(CROSSPOINT_SD_FONTS)
#include <builtinFonts/merriweather_11_bold.h>
#include <builtinFonts/merriweather_11_italic.h>
#include <builtinFonts/merriweather_11_regular.h>
#include <builtinFonts/merriweather_12_bold.h>
#include <builtinFonts/merriweather_12_italic.h>
#include <builtinFonts/merriweather_12_regular.h>
#include <builtinFonts/merriweather_13_bold.h>
#include <builtinFonts/merriweather_13_italic.h>
#include <builtinFonts/merriweather_13_regular.h>
#include <builtinFonts/merriweather_14_bold.h>
#include <builtinFonts/merriweather_14_italic.h>
#include <builtinFonts/merriweather_14_regular.h>
#include <builtinFonts/merriweather_15_bold.h>
#include <builtinFonts/merriweather_15_italic.h>
#include <builtinFonts/merriweather_15_regular.h>
#include <builtinFonts/merriweather_16_bold.h>
#include <builtinFonts/merriweather_16_italic.h>
#include <builtinFonts/merriweather_16_regular.h>
#include <builtinFonts/merriweather_17_bold.h>
#include <builtinFonts/merriweather_17_italic.h>
#include <builtinFonts/merriweather_17_regular.h>
#endif
#include "lifecycle/ActivityRouter.h"
#include "network/WifiDiagReport.h"
#include "persist/AppStateStore.h"
#include "persist/AsyncWriter.h"
#include "persist/PersistManager.h"
#include "persist/SdFatFileIO.h"
#include "persist/Trash.h"
#include "sleep/Wallpaper.h"
#include "util/ButtonNavigator.h"
#include "util/TransitionFeedback.h"

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
FontDecompressor fontDecompressor;
Activity* currentActivity = nullptr;

// Fonts
// Lector: ChareInk fully removed. Bookerly is the reader default + the missing-glyph
// fallback + the OOM emergency-degrade floor (Bookerly-11). Its glyph arrays from
// all.h are now unreferenced and GC'd by --gc-sections.
// Size 10 of the 5 flash families is dropped from the picker in the flash-extra
// build (smallest selectable size becomes 11), so its bitmaps are not baked there
// — reclaims ~358 KB of flash across the 5 families (the static glyph arrays from
// all.h become unreferenced and are GC'd by --gc-sections). Still compiled for SD
// builds (9-size set keeps 10) and for plain builds (5-size set keeps 10). ChareInk
// size 10 is unaffected — it stays as the always-in-flash fallback floor.
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)
EpdFont bookerly10RegularFont(&bookerly_10_regular);
EpdFont bookerly10BoldFont(&bookerly_10_bold);
EpdFont bookerly10ItalicFont(&bookerly_10_italic);
EpdFontFamily bookerly10FontFamily(&bookerly10RegularFont, &bookerly10BoldFont, &bookerly10ItalicFont, nullptr);
#endif  // !CROSSPOINT_FLASH_EXTRA_SIZES
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont, nullptr);
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont, nullptr);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont, nullptr);
EpdFont bookerly17RegularFont(&bookerly_17_regular);
EpdFont bookerly17BoldFont(&bookerly_17_bold);
EpdFont bookerly17ItalicFont(&bookerly_17_italic);
EpdFontFamily bookerly17FontFamily(&bookerly17RegularFont, &bookerly17BoldFont, &bookerly17ItalicFont, nullptr);
// Georgia: serif reader font. Regular, Bold, Italic (no BoldItalic source --
// slot nullptr, renderer synthesises bold-italic). Sizes 10, 12, 14, 16, 17. Ships full
// prose punctuation, so no source patching needed (unlike F25).
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
EpdFont georgia_10RegularFont(&georgia_10_regular);
EpdFont georgia_10BoldFont(&georgia_10_bold);
EpdFont georgia_10ItalicFont(&georgia_10_italic);
EpdFontFamily georgia_10FontFamily(&georgia_10RegularFont, &georgia_10BoldFont, &georgia_10ItalicFont, nullptr);
#endif  // !CROSSPOINT_FLASH_EXTRA_SIZES
EpdFont georgia_12RegularFont(&georgia_12_regular);
EpdFont georgia_12BoldFont(&georgia_12_bold);
EpdFont georgia_12ItalicFont(&georgia_12_italic);
EpdFontFamily georgia_12FontFamily(&georgia_12RegularFont, &georgia_12BoldFont, &georgia_12ItalicFont, nullptr);
EpdFont georgia_14RegularFont(&georgia_14_regular);
EpdFont georgia_14BoldFont(&georgia_14_bold);
EpdFont georgia_14ItalicFont(&georgia_14_italic);
EpdFontFamily georgia_14FontFamily(&georgia_14RegularFont, &georgia_14BoldFont, &georgia_14ItalicFont, nullptr);
EpdFont georgia_16RegularFont(&georgia_16_regular);
EpdFont georgia_16BoldFont(&georgia_16_bold);
EpdFont georgia_16ItalicFont(&georgia_16_italic);
EpdFontFamily georgia_16FontFamily(&georgia_16RegularFont, &georgia_16BoldFont, &georgia_16ItalicFont, nullptr);
EpdFont georgia_17RegularFont(&georgia_17_regular);
EpdFont georgia_17BoldFont(&georgia_17_bold);
EpdFont georgia_17ItalicFont(&georgia_17_italic);
EpdFontFamily georgia_17FontFamily(&georgia_17RegularFont, &georgia_17BoldFont, &georgia_17ItalicFont, nullptr);

// Lato removed (Lector) — no longer a reader font; UI font is now Cozette.

// Helvetica: grotesque sans-serif reader font. Regular, Bold, Italic (the
// macOS Helvetica Oblique face). No BoldItalic source -> nullptr slot, renderer
// synthesises bold-italic (Georgia pattern). Sizes 10, 12, 14, 16, 17.
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
EpdFont helvetica_10RegularFont(&helvetica_10_regular);
EpdFont helvetica_10BoldFont(&helvetica_10_bold);
EpdFont helvetica_10ItalicFont(&helvetica_10_italic);
EpdFontFamily helvetica_10FontFamily(&helvetica_10RegularFont, &helvetica_10BoldFont, &helvetica_10ItalicFont, nullptr);
#endif  // !CROSSPOINT_FLASH_EXTRA_SIZES
EpdFont helvetica_12RegularFont(&helvetica_12_regular);
EpdFont helvetica_12BoldFont(&helvetica_12_bold);
EpdFont helvetica_12ItalicFont(&helvetica_12_italic);
EpdFontFamily helvetica_12FontFamily(&helvetica_12RegularFont, &helvetica_12BoldFont, &helvetica_12ItalicFont, nullptr);
EpdFont helvetica_14RegularFont(&helvetica_14_regular);
EpdFont helvetica_14BoldFont(&helvetica_14_bold);
EpdFont helvetica_14ItalicFont(&helvetica_14_italic);
EpdFontFamily helvetica_14FontFamily(&helvetica_14RegularFont, &helvetica_14BoldFont, &helvetica_14ItalicFont, nullptr);
EpdFont helvetica_16RegularFont(&helvetica_16_regular);
EpdFont helvetica_16BoldFont(&helvetica_16_bold);
EpdFont helvetica_16ItalicFont(&helvetica_16_italic);
EpdFontFamily helvetica_16FontFamily(&helvetica_16RegularFont, &helvetica_16BoldFont, &helvetica_16ItalicFont, nullptr);
EpdFont helvetica_17RegularFont(&helvetica_17_regular);
EpdFont helvetica_17BoldFont(&helvetica_17_bold);
EpdFont helvetica_17ItalicFont(&helvetica_17_italic);
EpdFontFamily helvetica_17FontFamily(&helvetica_17RegularFont, &helvetica_17BoldFont, &helvetica_17ItalicFont, nullptr);

// Verdana: humanist sans-serif reader font (wide, screen-legible). Regular,
// Bold, Italic. No BoldItalic source -> nullptr slot, renderer synthesises
// bold-italic (Georgia pattern). Sizes 10, 12, 14, 16, 17.
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
EpdFont verdana_10RegularFont(&verdana_10_regular);
EpdFont verdana_10BoldFont(&verdana_10_bold);
EpdFont verdana_10ItalicFont(&verdana_10_italic);
EpdFontFamily verdana_10FontFamily(&verdana_10RegularFont, &verdana_10BoldFont, &verdana_10ItalicFont, nullptr);
#endif  // !CROSSPOINT_FLASH_EXTRA_SIZES
EpdFont verdana_12RegularFont(&verdana_12_regular);
EpdFont verdana_12BoldFont(&verdana_12_bold);
EpdFont verdana_12ItalicFont(&verdana_12_italic);
EpdFontFamily verdana_12FontFamily(&verdana_12RegularFont, &verdana_12BoldFont, &verdana_12ItalicFont, nullptr);
EpdFont verdana_14RegularFont(&verdana_14_regular);
EpdFont verdana_14BoldFont(&verdana_14_bold);
EpdFont verdana_14ItalicFont(&verdana_14_italic);
EpdFontFamily verdana_14FontFamily(&verdana_14RegularFont, &verdana_14BoldFont, &verdana_14ItalicFont, nullptr);
EpdFont verdana_16RegularFont(&verdana_16_regular);
EpdFont verdana_16BoldFont(&verdana_16_bold);
EpdFont verdana_16ItalicFont(&verdana_16_italic);
EpdFontFamily verdana_16FontFamily(&verdana_16RegularFont, &verdana_16BoldFont, &verdana_16ItalicFont, nullptr);
EpdFont verdana_17RegularFont(&verdana_17_regular);
EpdFont verdana_17BoldFont(&verdana_17_bold);
EpdFont verdana_17ItalicFont(&verdana_17_italic);
EpdFontFamily verdana_17FontFamily(&verdana_17RegularFont, &verdana_17BoldFont, &verdana_17ItalicFont, nullptr);

// Reader sizes 11/13/15 — real flash fonts (tables in flash; bitmaps streamed from
// SD in SD builds). Baked into the flash build via CROSSPOINT_FLASH_EXTRA_SIZES so
// the size picker offers {11,12,13,14,15,16,17}. Size 18 of these families + the
// SD-only families stay SD-only (gated separately below).
#if defined(CROSSPOINT_SD_FONTS) || defined(CROSSPOINT_FLASH_EXTRA_SIZES)
// Bookerly extra sizes.
EpdFont bookerly_11RegularFont(&bookerly_11_regular);
EpdFont bookerly_11BoldFont(&bookerly_11_bold);
EpdFont bookerly_11ItalicFont(&bookerly_11_italic);
EpdFontFamily bookerly_11FontFamily(&bookerly_11RegularFont, &bookerly_11BoldFont, &bookerly_11ItalicFont, nullptr);
EpdFont bookerly_13RegularFont(&bookerly_13_regular);
EpdFont bookerly_13BoldFont(&bookerly_13_bold);
EpdFont bookerly_13ItalicFont(&bookerly_13_italic);
EpdFontFamily bookerly_13FontFamily(&bookerly_13RegularFont, &bookerly_13BoldFont, &bookerly_13ItalicFont, nullptr);
EpdFont bookerly_15RegularFont(&bookerly_15_regular);
EpdFont bookerly_15BoldFont(&bookerly_15_bold);
EpdFont bookerly_15ItalicFont(&bookerly_15_italic);
EpdFontFamily bookerly_15FontFamily(&bookerly_15RegularFont, &bookerly_15BoldFont, &bookerly_15ItalicFont, nullptr);
// Georgia extra sizes.
EpdFont georgia_11RegularFont(&georgia_11_regular);
EpdFont georgia_11BoldFont(&georgia_11_bold);
EpdFont georgia_11ItalicFont(&georgia_11_italic);
EpdFontFamily georgia_11FontFamily(&georgia_11RegularFont, &georgia_11BoldFont, &georgia_11ItalicFont, nullptr);
EpdFont georgia_13RegularFont(&georgia_13_regular);
EpdFont georgia_13BoldFont(&georgia_13_bold);
EpdFont georgia_13ItalicFont(&georgia_13_italic);
EpdFontFamily georgia_13FontFamily(&georgia_13RegularFont, &georgia_13BoldFont, &georgia_13ItalicFont, nullptr);
EpdFont georgia_15RegularFont(&georgia_15_regular);
EpdFont georgia_15BoldFont(&georgia_15_bold);
EpdFont georgia_15ItalicFont(&georgia_15_italic);
EpdFontFamily georgia_15FontFamily(&georgia_15RegularFont, &georgia_15BoldFont, &georgia_15ItalicFont, nullptr);
// Lato extra sizes removed (Lector).
// Helvetica extra sizes.
EpdFont helvetica_11RegularFont(&helvetica_11_regular);
EpdFont helvetica_11BoldFont(&helvetica_11_bold);
EpdFont helvetica_11ItalicFont(&helvetica_11_italic);
EpdFontFamily helvetica_11FontFamily(&helvetica_11RegularFont, &helvetica_11BoldFont, &helvetica_11ItalicFont, nullptr);
EpdFont helvetica_13RegularFont(&helvetica_13_regular);
EpdFont helvetica_13BoldFont(&helvetica_13_bold);
EpdFont helvetica_13ItalicFont(&helvetica_13_italic);
EpdFontFamily helvetica_13FontFamily(&helvetica_13RegularFont, &helvetica_13BoldFont, &helvetica_13ItalicFont, nullptr);
EpdFont helvetica_15RegularFont(&helvetica_15_regular);
EpdFont helvetica_15BoldFont(&helvetica_15_bold);
EpdFont helvetica_15ItalicFont(&helvetica_15_italic);
EpdFontFamily helvetica_15FontFamily(&helvetica_15RegularFont, &helvetica_15BoldFont, &helvetica_15ItalicFont, nullptr);
// Verdana extra sizes.
EpdFont verdana_11RegularFont(&verdana_11_regular);
EpdFont verdana_11BoldFont(&verdana_11_bold);
EpdFont verdana_11ItalicFont(&verdana_11_italic);
EpdFontFamily verdana_11FontFamily(&verdana_11RegularFont, &verdana_11BoldFont, &verdana_11ItalicFont, nullptr);
EpdFont verdana_13RegularFont(&verdana_13_regular);
EpdFont verdana_13BoldFont(&verdana_13_bold);
EpdFont verdana_13ItalicFont(&verdana_13_italic);
EpdFontFamily verdana_13FontFamily(&verdana_13RegularFont, &verdana_13BoldFont, &verdana_13ItalicFont, nullptr);
EpdFont verdana_15RegularFont(&verdana_15_regular);
EpdFont verdana_15BoldFont(&verdana_15_bold);
EpdFont verdana_15ItalicFont(&verdana_15_italic);
EpdFontFamily verdana_15FontFamily(&verdana_15RegularFont, &verdana_15BoldFont, &verdana_15ItalicFont, nullptr);
#endif  // flash extra sizes 11/13/15

// Lector: Merriweather baked into flash as a 5th reader family, sizes 11..17,
// Regular/Bold/Italic (no BoldItalic — renderer synthesises it). In SD builds
// Merriweather lives in the CROSSPOINT_SD_FONTS block below instead, so gate this
// to the flash-only regime to avoid a double definition.
#if defined(CROSSPOINT_FLASH_EXTRA_SIZES) && !defined(CROSSPOINT_SD_FONTS)
EpdFont merriweather_11RegularFont(&merriweather_11_regular);
EpdFont merriweather_11BoldFont(&merriweather_11_bold);
EpdFont merriweather_11ItalicFont(&merriweather_11_italic);
EpdFontFamily merriweather_11FontFamily(&merriweather_11RegularFont, &merriweather_11BoldFont,
                                        &merriweather_11ItalicFont, nullptr);
EpdFont merriweather_12RegularFont(&merriweather_12_regular);
EpdFont merriweather_12BoldFont(&merriweather_12_bold);
EpdFont merriweather_12ItalicFont(&merriweather_12_italic);
EpdFontFamily merriweather_12FontFamily(&merriweather_12RegularFont, &merriweather_12BoldFont,
                                        &merriweather_12ItalicFont, nullptr);
EpdFont merriweather_13RegularFont(&merriweather_13_regular);
EpdFont merriweather_13BoldFont(&merriweather_13_bold);
EpdFont merriweather_13ItalicFont(&merriweather_13_italic);
EpdFontFamily merriweather_13FontFamily(&merriweather_13RegularFont, &merriweather_13BoldFont,
                                        &merriweather_13ItalicFont, nullptr);
EpdFont merriweather_14RegularFont(&merriweather_14_regular);
EpdFont merriweather_14BoldFont(&merriweather_14_bold);
EpdFont merriweather_14ItalicFont(&merriweather_14_italic);
EpdFontFamily merriweather_14FontFamily(&merriweather_14RegularFont, &merriweather_14BoldFont,
                                        &merriweather_14ItalicFont, nullptr);
EpdFont merriweather_15RegularFont(&merriweather_15_regular);
EpdFont merriweather_15BoldFont(&merriweather_15_bold);
EpdFont merriweather_15ItalicFont(&merriweather_15_italic);
EpdFontFamily merriweather_15FontFamily(&merriweather_15RegularFont, &merriweather_15BoldFont,
                                        &merriweather_15ItalicFont, nullptr);
EpdFont merriweather_16RegularFont(&merriweather_16_regular);
EpdFont merriweather_16BoldFont(&merriweather_16_bold);
EpdFont merriweather_16ItalicFont(&merriweather_16_italic);
EpdFontFamily merriweather_16FontFamily(&merriweather_16RegularFont, &merriweather_16BoldFont,
                                        &merriweather_16ItalicFont, nullptr);
EpdFont merriweather_17RegularFont(&merriweather_17_regular);
EpdFont merriweather_17BoldFont(&merriweather_17_bold);
EpdFont merriweather_17ItalicFont(&merriweather_17_italic);
EpdFontFamily merriweather_17FontFamily(&merriweather_17RegularFont, &merriweather_17BoldFont,
                                        &merriweather_17ItalicFont, nullptr);
#endif  // flash Merriweather 11..17

#ifdef CROSSPOINT_SD_FONTS
// SD-only Tier-1: size 18 of the 5 flash families + (below) the SD-only families.
// Never baked into the flash build (picker tops out at 17 there).
EpdFont bookerly_18RegularFont(&bookerly_18_regular);
EpdFont bookerly_18BoldFont(&bookerly_18_bold);
EpdFont bookerly_18ItalicFont(&bookerly_18_italic);
EpdFontFamily bookerly_18FontFamily(&bookerly_18RegularFont, &bookerly_18BoldFont, &bookerly_18ItalicFont, nullptr);
EpdFont georgia_18RegularFont(&georgia_18_regular);
EpdFont georgia_18BoldFont(&georgia_18_bold);
EpdFont georgia_18ItalicFont(&georgia_18_italic);
EpdFontFamily georgia_18FontFamily(&georgia_18RegularFont, &georgia_18BoldFont, &georgia_18ItalicFont, nullptr);
EpdFont lato_18RegularFont(&lato_18_regular);
EpdFont lato_18BoldFont(&lato_18_bold);
EpdFont lato_18ItalicFont(&lato_18_italic);
EpdFont lato_18BoldItalicFont(&lato_18_bolditalic);
EpdFontFamily lato_18FontFamily(&lato_18RegularFont, &lato_18BoldFont, &lato_18ItalicFont, &lato_18BoldItalicFont);
EpdFont helvetica_18RegularFont(&helvetica_18_regular);
EpdFont helvetica_18BoldFont(&helvetica_18_bold);
EpdFont helvetica_18ItalicFont(&helvetica_18_italic);
EpdFontFamily helvetica_18FontFamily(&helvetica_18RegularFont, &helvetica_18BoldFont, &helvetica_18ItalicFont, nullptr);
EpdFont verdana_18RegularFont(&verdana_18_regular);
EpdFont verdana_18BoldFont(&verdana_18_bold);
EpdFont verdana_18ItalicFont(&verdana_18_italic);
EpdFontFamily verdana_18FontFamily(&verdana_18RegularFont, &verdana_18BoldFont, &verdana_18ItalicFont, nullptr);
// Merriweather: serif reader font (SD-only Tier-1, 4 real weights). Sizes 10..18.
EpdFont merriweather_10RegularFont(&merriweather_10_regular);
EpdFont merriweather_10BoldFont(&merriweather_10_bold);
EpdFont merriweather_10ItalicFont(&merriweather_10_italic);
EpdFont merriweather_10BoldItalicFont(&merriweather_10_bolditalic);
EpdFontFamily merriweather_10FontFamily(&merriweather_10RegularFont, &merriweather_10BoldFont,
                                        &merriweather_10ItalicFont, &merriweather_10BoldItalicFont);
EpdFont merriweather_11RegularFont(&merriweather_11_regular);
EpdFont merriweather_11BoldFont(&merriweather_11_bold);
EpdFont merriweather_11ItalicFont(&merriweather_11_italic);
EpdFont merriweather_11BoldItalicFont(&merriweather_11_bolditalic);
EpdFontFamily merriweather_11FontFamily(&merriweather_11RegularFont, &merriweather_11BoldFont,
                                        &merriweather_11ItalicFont, &merriweather_11BoldItalicFont);
EpdFont merriweather_12RegularFont(&merriweather_12_regular);
EpdFont merriweather_12BoldFont(&merriweather_12_bold);
EpdFont merriweather_12ItalicFont(&merriweather_12_italic);
EpdFont merriweather_12BoldItalicFont(&merriweather_12_bolditalic);
EpdFontFamily merriweather_12FontFamily(&merriweather_12RegularFont, &merriweather_12BoldFont,
                                        &merriweather_12ItalicFont, &merriweather_12BoldItalicFont);
EpdFont merriweather_13RegularFont(&merriweather_13_regular);
EpdFont merriweather_13BoldFont(&merriweather_13_bold);
EpdFont merriweather_13ItalicFont(&merriweather_13_italic);
EpdFont merriweather_13BoldItalicFont(&merriweather_13_bolditalic);
EpdFontFamily merriweather_13FontFamily(&merriweather_13RegularFont, &merriweather_13BoldFont,
                                        &merriweather_13ItalicFont, &merriweather_13BoldItalicFont);
EpdFont merriweather_14RegularFont(&merriweather_14_regular);
EpdFont merriweather_14BoldFont(&merriweather_14_bold);
EpdFont merriweather_14ItalicFont(&merriweather_14_italic);
EpdFont merriweather_14BoldItalicFont(&merriweather_14_bolditalic);
EpdFontFamily merriweather_14FontFamily(&merriweather_14RegularFont, &merriweather_14BoldFont,
                                        &merriweather_14ItalicFont, &merriweather_14BoldItalicFont);
EpdFont merriweather_15RegularFont(&merriweather_15_regular);
EpdFont merriweather_15BoldFont(&merriweather_15_bold);
EpdFont merriweather_15ItalicFont(&merriweather_15_italic);
EpdFont merriweather_15BoldItalicFont(&merriweather_15_bolditalic);
EpdFontFamily merriweather_15FontFamily(&merriweather_15RegularFont, &merriweather_15BoldFont,
                                        &merriweather_15ItalicFont, &merriweather_15BoldItalicFont);
EpdFont merriweather_16RegularFont(&merriweather_16_regular);
EpdFont merriweather_16BoldFont(&merriweather_16_bold);
EpdFont merriweather_16ItalicFont(&merriweather_16_italic);
EpdFont merriweather_16BoldItalicFont(&merriweather_16_bolditalic);
EpdFontFamily merriweather_16FontFamily(&merriweather_16RegularFont, &merriweather_16BoldFont,
                                        &merriweather_16ItalicFont, &merriweather_16BoldItalicFont);
EpdFont merriweather_17RegularFont(&merriweather_17_regular);
EpdFont merriweather_17BoldFont(&merriweather_17_bold);
EpdFont merriweather_17ItalicFont(&merriweather_17_italic);
EpdFont merriweather_17BoldItalicFont(&merriweather_17_bolditalic);
EpdFontFamily merriweather_17FontFamily(&merriweather_17RegularFont, &merriweather_17BoldFont,
                                        &merriweather_17ItalicFont, &merriweather_17BoldItalicFont);
EpdFont merriweather_18RegularFont(&merriweather_18_regular);
EpdFont merriweather_18BoldFont(&merriweather_18_bold);
EpdFont merriweather_18ItalicFont(&merriweather_18_italic);
EpdFont merriweather_18BoldItalicFont(&merriweather_18_bolditalic);
EpdFontFamily merriweather_18FontFamily(&merriweather_18RegularFont, &merriweather_18BoldFont,
                                        &merriweather_18ItalicFont, &merriweather_18BoldItalicFont);
// Playfair Display: high-contrast serif (SD-only Tier-1, 4 real weights). Sizes 10..18.
EpdFont playfair_10RegularFont(&playfair_10_regular);
EpdFont playfair_10BoldFont(&playfair_10_bold);
EpdFont playfair_10ItalicFont(&playfair_10_italic);
EpdFont playfair_10BoldItalicFont(&playfair_10_bolditalic);
EpdFontFamily playfair_10FontFamily(&playfair_10RegularFont, &playfair_10BoldFont, &playfair_10ItalicFont,
                                    &playfair_10BoldItalicFont);
EpdFont playfair_11RegularFont(&playfair_11_regular);
EpdFont playfair_11BoldFont(&playfair_11_bold);
EpdFont playfair_11ItalicFont(&playfair_11_italic);
EpdFont playfair_11BoldItalicFont(&playfair_11_bolditalic);
EpdFontFamily playfair_11FontFamily(&playfair_11RegularFont, &playfair_11BoldFont, &playfair_11ItalicFont,
                                    &playfair_11BoldItalicFont);
EpdFont playfair_12RegularFont(&playfair_12_regular);
EpdFont playfair_12BoldFont(&playfair_12_bold);
EpdFont playfair_12ItalicFont(&playfair_12_italic);
EpdFont playfair_12BoldItalicFont(&playfair_12_bolditalic);
EpdFontFamily playfair_12FontFamily(&playfair_12RegularFont, &playfair_12BoldFont, &playfair_12ItalicFont,
                                    &playfair_12BoldItalicFont);
EpdFont playfair_13RegularFont(&playfair_13_regular);
EpdFont playfair_13BoldFont(&playfair_13_bold);
EpdFont playfair_13ItalicFont(&playfair_13_italic);
EpdFont playfair_13BoldItalicFont(&playfair_13_bolditalic);
EpdFontFamily playfair_13FontFamily(&playfair_13RegularFont, &playfair_13BoldFont, &playfair_13ItalicFont,
                                    &playfair_13BoldItalicFont);
EpdFont playfair_14RegularFont(&playfair_14_regular);
EpdFont playfair_14BoldFont(&playfair_14_bold);
EpdFont playfair_14ItalicFont(&playfair_14_italic);
EpdFont playfair_14BoldItalicFont(&playfair_14_bolditalic);
EpdFontFamily playfair_14FontFamily(&playfair_14RegularFont, &playfair_14BoldFont, &playfair_14ItalicFont,
                                    &playfair_14BoldItalicFont);
EpdFont playfair_15RegularFont(&playfair_15_regular);
EpdFont playfair_15BoldFont(&playfair_15_bold);
EpdFont playfair_15ItalicFont(&playfair_15_italic);
EpdFont playfair_15BoldItalicFont(&playfair_15_bolditalic);
EpdFontFamily playfair_15FontFamily(&playfair_15RegularFont, &playfair_15BoldFont, &playfair_15ItalicFont,
                                    &playfair_15BoldItalicFont);
EpdFont playfair_16RegularFont(&playfair_16_regular);
EpdFont playfair_16BoldFont(&playfair_16_bold);
EpdFont playfair_16ItalicFont(&playfair_16_italic);
EpdFont playfair_16BoldItalicFont(&playfair_16_bolditalic);
EpdFontFamily playfair_16FontFamily(&playfair_16RegularFont, &playfair_16BoldFont, &playfair_16ItalicFont,
                                    &playfair_16BoldItalicFont);
EpdFont playfair_17RegularFont(&playfair_17_regular);
EpdFont playfair_17BoldFont(&playfair_17_bold);
EpdFont playfair_17ItalicFont(&playfair_17_italic);
EpdFont playfair_17BoldItalicFont(&playfair_17_bolditalic);
EpdFontFamily playfair_17FontFamily(&playfair_17RegularFont, &playfair_17BoldFont, &playfair_17ItalicFont,
                                    &playfair_17BoldItalicFont);
EpdFont playfair_18RegularFont(&playfair_18_regular);
EpdFont playfair_18BoldFont(&playfair_18_bold);
EpdFont playfair_18ItalicFont(&playfair_18_italic);
EpdFont playfair_18BoldItalicFont(&playfair_18_bolditalic);
EpdFontFamily playfair_18FontFamily(&playfair_18RegularFont, &playfair_18BoldFont, &playfair_18ItalicFont,
                                    &playfair_18BoldItalicFont);
// Galmuri: SD-only pixel font. Native crisp sizes 14px (1x) + 28px (2x), 3 weights
// (bold-italic synthesised). Reader maps the 10..18 scale onto these two sizes.
EpdFont galmuri_14RegularFont(&galmuri_14_regular);
EpdFont galmuri_14BoldFont(&galmuri_14_bold);
EpdFont galmuri_14ItalicFont(&galmuri_14_italic);
EpdFontFamily galmuri_14FontFamily(&galmuri_14RegularFont, &galmuri_14BoldFont, &galmuri_14ItalicFont, nullptr);
EpdFont galmuri_28RegularFont(&galmuri_28_regular);
EpdFont galmuri_28BoldFont(&galmuri_28_bold);
EpdFont galmuri_28ItalicFont(&galmuri_28_italic);
EpdFontFamily galmuri_28FontFamily(&galmuri_28RegularFont, &galmuri_28BoldFont, &galmuri_28ItalicFont, nullptr);
// Vollkorn: serif reader font (SD-only Tier-1, 4 real weights, real kerning). Sizes 10..18.
EpdFont vollkorn_10RegularFont(&vollkorn_10_regular);
EpdFont vollkorn_10BoldFont(&vollkorn_10_bold);
EpdFont vollkorn_10ItalicFont(&vollkorn_10_italic);
EpdFont vollkorn_10BoldItalicFont(&vollkorn_10_bolditalic);
EpdFontFamily vollkorn_10FontFamily(&vollkorn_10RegularFont, &vollkorn_10BoldFont, &vollkorn_10ItalicFont,
                                    &vollkorn_10BoldItalicFont);
EpdFont vollkorn_11RegularFont(&vollkorn_11_regular);
EpdFont vollkorn_11BoldFont(&vollkorn_11_bold);
EpdFont vollkorn_11ItalicFont(&vollkorn_11_italic);
EpdFont vollkorn_11BoldItalicFont(&vollkorn_11_bolditalic);
EpdFontFamily vollkorn_11FontFamily(&vollkorn_11RegularFont, &vollkorn_11BoldFont, &vollkorn_11ItalicFont,
                                    &vollkorn_11BoldItalicFont);
EpdFont vollkorn_12RegularFont(&vollkorn_12_regular);
EpdFont vollkorn_12BoldFont(&vollkorn_12_bold);
EpdFont vollkorn_12ItalicFont(&vollkorn_12_italic);
EpdFont vollkorn_12BoldItalicFont(&vollkorn_12_bolditalic);
EpdFontFamily vollkorn_12FontFamily(&vollkorn_12RegularFont, &vollkorn_12BoldFont, &vollkorn_12ItalicFont,
                                    &vollkorn_12BoldItalicFont);
EpdFont vollkorn_13RegularFont(&vollkorn_13_regular);
EpdFont vollkorn_13BoldFont(&vollkorn_13_bold);
EpdFont vollkorn_13ItalicFont(&vollkorn_13_italic);
EpdFont vollkorn_13BoldItalicFont(&vollkorn_13_bolditalic);
EpdFontFamily vollkorn_13FontFamily(&vollkorn_13RegularFont, &vollkorn_13BoldFont, &vollkorn_13ItalicFont,
                                    &vollkorn_13BoldItalicFont);
EpdFont vollkorn_14RegularFont(&vollkorn_14_regular);
EpdFont vollkorn_14BoldFont(&vollkorn_14_bold);
EpdFont vollkorn_14ItalicFont(&vollkorn_14_italic);
EpdFont vollkorn_14BoldItalicFont(&vollkorn_14_bolditalic);
EpdFontFamily vollkorn_14FontFamily(&vollkorn_14RegularFont, &vollkorn_14BoldFont, &vollkorn_14ItalicFont,
                                    &vollkorn_14BoldItalicFont);
EpdFont vollkorn_15RegularFont(&vollkorn_15_regular);
EpdFont vollkorn_15BoldFont(&vollkorn_15_bold);
EpdFont vollkorn_15ItalicFont(&vollkorn_15_italic);
EpdFont vollkorn_15BoldItalicFont(&vollkorn_15_bolditalic);
EpdFontFamily vollkorn_15FontFamily(&vollkorn_15RegularFont, &vollkorn_15BoldFont, &vollkorn_15ItalicFont,
                                    &vollkorn_15BoldItalicFont);
EpdFont vollkorn_16RegularFont(&vollkorn_16_regular);
EpdFont vollkorn_16BoldFont(&vollkorn_16_bold);
EpdFont vollkorn_16ItalicFont(&vollkorn_16_italic);
EpdFont vollkorn_16BoldItalicFont(&vollkorn_16_bolditalic);
EpdFontFamily vollkorn_16FontFamily(&vollkorn_16RegularFont, &vollkorn_16BoldFont, &vollkorn_16ItalicFont,
                                    &vollkorn_16BoldItalicFont);
EpdFont vollkorn_17RegularFont(&vollkorn_17_regular);
EpdFont vollkorn_17BoldFont(&vollkorn_17_bold);
EpdFont vollkorn_17ItalicFont(&vollkorn_17_italic);
EpdFont vollkorn_17BoldItalicFont(&vollkorn_17_bolditalic);
EpdFontFamily vollkorn_17FontFamily(&vollkorn_17RegularFont, &vollkorn_17BoldFont, &vollkorn_17ItalicFont,
                                    &vollkorn_17BoldItalicFont);
EpdFont vollkorn_18RegularFont(&vollkorn_18_regular);
EpdFont vollkorn_18BoldFont(&vollkorn_18_bold);
EpdFont vollkorn_18ItalicFont(&vollkorn_18_italic);
EpdFont vollkorn_18BoldItalicFont(&vollkorn_18_bolditalic);
EpdFontFamily vollkorn_18FontFamily(&vollkorn_18RegularFont, &vollkorn_18BoldFont, &vollkorn_18ItalicFont,
                                    &vollkorn_18BoldItalicFont);
#endif  // CROSSPOINT_SD_FONTS

// Lector UI font = Cozette at v11.0.0's three distinct sizes: ui_8 = status bar
// (SMALL_FONT_ID), ui_10 = body/menus (UI_10_FONT_ID), ui_12 = titles/headers
// (UI_12_FONT_ID, distinct). Regular only — visual hierarchy is size, not weight.
EpdFont smallFont(&ui_8_regular);
EpdFontFamily smallFontFamily(&smallFont, nullptr, nullptr, nullptr, 0, 0, false);
EpdFont uiRegularFont(&ui_10_regular);
EpdFontFamily uiFontFamily(&uiRegularFont, nullptr, nullptr, nullptr, 0, 0, false);
EpdFont uiTitleFont(&ui_12_regular);
EpdFontFamily ui12FontFamily(&uiTitleFont, nullptr, nullptr, nullptr, 0, 0, false);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void onGoHome();  // fwd decl — defined later, needed by OOM fallback below

void enterNewActivity(Activity* activity) {
  // Suppress stale button events from the previous activity so that
  // a press/release that closed the old screen doesn't leak into the new one.
  gpio.suppressUntilAllReleased();
  // Guarantee any transition popup drawn by the route factory has been
  // visible for the floor duration before the destination's first render
  // (forced HALF_REFRESH via requestHalfRefresh in show()) overwrites it.
  // On warm/cached destinations onEnter() reaches displayBuffer too quickly
  // and the popup is wiped before the user perceives it.
  TransitionFeedback::ensureMinDisplayElapsed();
  if (activity) {
    currentActivity = activity;
    currentActivity->onEnter();
    if (!currentActivity->didEntryFail()) {
      return;
    }
    // Activity could not bring itself up (semaphore alloc, low-largest-block,
    // or xTaskCreate failure). Replace it with a graceful OOM screen instead
    // of letting the user see a silent reboot.
    LOG_ERR("MAIN", "Activity entry failed — swapping to OOM screen");
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  } else {
    // The activity allocation itself failed: new (std::nothrow) returned null.
    // Fall through into the same recovery ladder as a failed onEnter() so a
    // route that cannot allocate degrades gracefully instead of null-derefing.
    LOG_ERR("MAIN", "Activity allocation failed — entering OOM recovery");
  }

  // If a book is open and we still have auto-restart budget, silent-restart
  // to the reader instead of showing the OOM screen. ESP32-C3 heap is
  // non-moving so the contiguous 8 KB FreeRTOS task stack that xTaskCreate
  // needs can't be recovered without a reboot; the silent reboot is the
  // root-cause fix. User sees the same brief screen flash as the WiFi-exit
  // silent reboot and lands back in the same book at the saved page.
  // The loop guard in tryReserveAutoSilentRestart bounds the attempts so a
  // chronically OOM-prone book falls through to the OOM screen below
  // instead of reboot-looping.
  if (!APP_STATE.openEpubPath.empty() && tryReserveAutoSilentRestart()) {
    LOG_DIAG("MAIN", "Activity entry OOM with book open — silent-restart-to-reader");
    silentRestartToReader("toplevel-activity-entry-oom");  // does not return
  }

  auto* oom = new (std::nothrow)
      FullScreenMessageActivity(renderer, mappedInputManager, "Out of memory\nPress any key", EpdFontFamily::REGULAR);
  if (oom) {
    oom->setOnDismiss(onGoHome);
    gpio.suppressUntilAllReleased();
    currentActivity = oom;
    currentActivity->onEnter();
    if (!currentActivity->didEntryFail()) {
      return;
    }
    // Even the OOM screen could not start — last-resort reboot.
    LOG_ERR("MAIN", "OOM fallback screen also failed to enter — restarting");
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  } else {
    LOG_ERR("MAIN", "OOM fallback allocation failed — restarting");
  }
  esp_restart();
}

bool persistAppState(const char* context) {
  // Force sync flush of all dirty stores. Activity transitions are the
  // crash-safety boundary — debounce only coalesces within an activity,
  // never across one.
  (void)context;
  // Drain any background SD writes (reader progress) first so the store
  // mutex isn't contended by a half-finished async write when we go to
  // commit app state.
  crosspoint::persist::AsyncWriter::instance().drainBlocking();
  const size_t flushed = crosspoint::persist::PersistManager().flushAll();
  (void)flushed;
  return true;
}

static void trimSleepFolderIfDirty();  // fwd decl — defined below

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const unsigned long calibration = start;
  const unsigned long targetDuration = SETTINGS.getPowerButtonDuration();
  const unsigned long calibratedPressDuration = (calibration < targetDuration) ? targetDuration - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
    esp_task_wdt_reset();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
      esp_task_wdt_reset();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < calibratedPressDuration);
    abort = gpio.getHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    powerManager.startDeepSleep(gpio);
  }
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

void onGoHome();
void onGoToMyLibraryWithPath(const std::string& path);
void onGoToRecentBooks();

// Sleep wallpaper rotation lives behind the wallpaper:: facade in
// src/sleep/Wallpaper.{h,cpp}. Production deps (fs, IFileIO, APP_STATE
// pointers, sync deep-sleep flush) are lazy-wired on first call. The boot
// route still calls trimSleepFolderIfDirty() — kept as a no-op shim so
// the ActivityRouter wiring stays valid; reconcile is heap-gated and runs
// from inside wallpaper::advance().

static void trimSleepFolderIfDirty() { crosspoint::sleep::wallpaper::reconcileIfDirty(); }

// Inline Reader activity construction. Used by both the V2 factory and the
// boot-time resume path in setup() (which bypasses the router since the main
// loop has not yet started draining pending transitions).
static void openReaderInline(const std::string& initialEpubPath) {
  const std::string bookPath = initialEpubPath;  // Copy before exitActivity() invalidates the reference
  // The "Opening book..." toast (with its resetStacking) is now drawn by
  // ReaderActivity::openBookPath — the single funnel for every open path,
  // including the in-reader recent-switcher that bypasses this function. Drawing
  // it here too would just FAST_REFRESH the same popup twice.
  exitActivity();
  enterNewActivity(new (std::nothrow)
                       ReaderActivity(renderer, mappedInputManager, bookPath, onGoHome, onGoToMyLibraryWithPath));
}

void onGoToReader(const std::string& initialEpubPath) {
  lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Reader, initialEpubPath});
}

static void openFileTransferInline() {
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_STARTING_SERVER));
  crosspoint::sleep::wallpaper::markFolderDirty();
  exitActivity();
  enterNewActivity(new (std::nothrow) CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToFileTransfer() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::FileTransfer, ""}); }

void onGoToSettings() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Settings, ""}); }

// ActivityRouter applies persist policy before calling this factory (RFC #23).
static void openMyLibraryInline(const std::string& path) {
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_LOADING_LIBRARY));
  exitActivity();
  if (path.empty()) {
    enterNewActivity(new (std::nothrow) MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
  } else {
    enterNewActivity(new (std::nothrow) MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader, path));
  }
}

void onGoToMyLibrary() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::MyLibrary, ""}); }

static void openRecentBooksInline() {
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_LOADING_RECENTS));
  exitActivity();
  enterNewActivity(new (std::nothrow) RecentBooksActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToRecentBooks() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::RecentBooks, ""}); }

void onGoToMyLibraryWithPath(const std::string& path) {
  lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::MyLibraryAt, path});
}

static void openBrowserInline() {
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_LOADING_BROWSER));
  exitActivity();
  enterNewActivity(new (std::nothrow) OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToBrowser() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Browser, ""}); }

static void openHomeInline() {
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_LOADING_HOME));
  exitActivity();
  enterNewActivity(new (std::nothrow)
                       HomeActivity(renderer, mappedInputManager, onGoToReader, onGoToMyLibrary, onGoToRecentBooks,
                                    onGoToSettings, onGoToFileTransfer, onGoToBrowser));
}

void onGoHome() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Home, ""}); }

// Wire ActivityRouter Deps + route factories (RFC #23). All transitions flow
// through the router: per-route persist/trim policy is applied before the
// factory runs, and deep-sleep entry follows the fixed hook sequence in
// ActivityRouter::enterDeepSleep. Lambdas capture file-scope statics
// (currentActivity, renderer, mappedInputManager, display, gpio, t1/t2,
// powerManager, APP_STATE) and the inline route openers above.
static void wireActivityRouter() {
  auto& router = lifecycle::ActivityRouter::instance();

  lifecycle::ActivityRouter::Deps deps;
  deps.currentActivitySlot = &currentActivity;
  deps.persistAppState = &::persistAppState;
  deps.trimSleepFolderIfDirty = &::trimSleepFolderIfDirty;
  deps.onBeforeDeepSleep = [](bool fromReader) { APP_STATE.lastSleepFromReader = fromReader; };
  deps.onAfterDeepSleep = []() {
    halTiltSensor.deepSleep();  // park the QMI8658 IMU (no-op on X4)
    display.deepSleep();
    LOG_DBG("MAIN", "Power button press calibration value: %lu ms", t2 - t1);
    LOG_DBG("MAIN", "Entering deep sleep");
    powerManager.startDeepSleep(gpio);
  };
  deps.enterSleepActivity = []() { enterNewActivity(new (std::nothrow) SleepActivity(renderer, mappedInputManager)); };
  router.setDeps(std::move(deps));

  router.setRouteFactory(lifecycle::RouteId::Settings, [](const std::string& /*payload*/) {
    TransitionFeedback::resetStacking();
    TransitionFeedback::show(renderer, tr(STR_LOADING_SETTINGS));
    exitActivity();
    enterNewActivity(new (std::nothrow) SettingsActivity(renderer, mappedInputManager, onGoHome));
  });
  router.setRouteFactory(lifecycle::RouteId::Reader, [](const std::string& payload) { openReaderInline(payload); });
  router.setRouteFactory(lifecycle::RouteId::MyLibrary,
                         [](const std::string& /*payload*/) { openMyLibraryInline(""); });
  router.setRouteFactory(lifecycle::RouteId::MyLibraryAt,
                         [](const std::string& payload) { openMyLibraryInline(payload); });
  router.setRouteFactory(lifecycle::RouteId::RecentBooks,
                         [](const std::string& /*payload*/) { openRecentBooksInline(); });
  router.setRouteFactory(lifecycle::RouteId::FileTransfer,
                         [](const std::string& /*payload*/) { openFileTransferInline(); });
  router.setRouteFactory(lifecycle::RouteId::Browser, [](const std::string& /*payload*/) { openBrowserInline(); });
  router.setRouteFactory(lifecycle::RouteId::Home, [](const std::string& /*payload*/) { openHomeInline(); });
}

// Heap allocator failure hook. ESP-IDF invokes this synchronously on
// the failing task before deciding whether to retry the allocation.
// Bare requirements: must not allocate (would re-enter), must be fast,
// must be callable from any task.
//
// We use it as the firmware's last-line defence against mid-render OOM
// — the FontDecompressor's hot-group buffer (~10 KB) and the section-
// rebuild parser are the two hot consumers that stress this path. By
// dropping the FCM cache here we hand back ~30-150 KB of hot-group +
// page-slot capacity that the failing allocation can then reuse on
// ESP-IDF's automatic retry.
//
// We do NOT touch CSS / page cache / activity-local state from here:
// those need locks and accessor pointers we can't safely take from
// arbitrary task context. FCM is a single-instance subsystem reachable
// through `renderer`, both globals at file scope, both fully
// constructed by the time the heap allocator is in use.

// The one SafeAnywhere shed evictor (RFC #163): alloc-free, lock-free,
// log-free, callable from the failed-alloc callback. Registered once at boot
// (setupDisplayAndFonts) into crosspoint::mem's registry, which both the
// reactive callback below and the dynamic mem::roomToGrow probe drain.
extern "C" void shedFontCacheEvictor() {
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
}

extern "C" void onHeapAllocFailed(size_t requested, uint32_t caps, const char* function_name) {
  // No std::string / no LOG_DIAG here — Logging may itself allocate.
  ets_printf("[DIAG] [HEAP] alloc-fail size=%u caps=%lu fn=%s\n", static_cast<unsigned>(requested),
             static_cast<unsigned long>(caps), function_name ? function_name : "?");
  crosspoint::mem::shedUnderPressure();
}

#ifdef CROSSPOINT_SD_FONTS
// Registers every offloadable reader-font family/size with the SD font manager.
// Each live EpdFont is paired with its same-size ChareInk regular as the fallback
// used only if an SD pack fails to load in a slim build (where the flash bitmap is
// gone), so the renderer never dereferences a dropped bitmap. The path stem mirrors
// the header basename, so e.g. BOOKERLY_17 -> /fonts/bookerly_17_<weight>.bin.
static void registerSdReaderFonts() {
  auto& m = crosspoint::fonts::sdFonts();
  m.registerFont(BOOKERLY_10_FONT_ID, 10, "bookerly", &bookerly10RegularFont, &bookerly10BoldFont,
                 &bookerly10ItalicFont, nullptr, &chareink_10_regular);
  m.registerFont(BOOKERLY_12_FONT_ID, 12, "bookerly", &bookerly12RegularFont, &bookerly12BoldFont,
                 &bookerly12ItalicFont, nullptr, &chareink_12_regular);
  m.registerFont(BOOKERLY_14_FONT_ID, 14, "bookerly", &bookerly14RegularFont, &bookerly14BoldFont,
                 &bookerly14ItalicFont, nullptr, &chareink_14_regular);
  m.registerFont(BOOKERLY_16_FONT_ID, 16, "bookerly", &bookerly16RegularFont, &bookerly16BoldFont,
                 &bookerly16ItalicFont, nullptr, &chareink_16_regular);
  m.registerFont(BOOKERLY_17_FONT_ID, 17, "bookerly", &bookerly17RegularFont, &bookerly17BoldFont,
                 &bookerly17ItalicFont, nullptr, &chareink_17_regular);

  m.registerFont(GEORGIA_10_FONT_ID, 10, "georgia", &georgia_10RegularFont, &georgia_10BoldFont, &georgia_10ItalicFont,
                 nullptr, &chareink_10_regular);
  m.registerFont(GEORGIA_12_FONT_ID, 12, "georgia", &georgia_12RegularFont, &georgia_12BoldFont, &georgia_12ItalicFont,
                 nullptr, &chareink_12_regular);
  m.registerFont(GEORGIA_14_FONT_ID, 14, "georgia", &georgia_14RegularFont, &georgia_14BoldFont, &georgia_14ItalicFont,
                 nullptr, &chareink_14_regular);
  m.registerFont(GEORGIA_16_FONT_ID, 16, "georgia", &georgia_16RegularFont, &georgia_16BoldFont, &georgia_16ItalicFont,
                 nullptr, &chareink_16_regular);
  m.registerFont(GEORGIA_17_FONT_ID, 17, "georgia", &georgia_17RegularFont, &georgia_17BoldFont, &georgia_17ItalicFont,
                 nullptr, &chareink_17_regular);

  m.registerFont(LATO_10_FONT_ID, 10, "lato", &lato_10RegularFont, &lato_10BoldFont, &lato_10ItalicFont,
                 &lato_10BoldItalicFont, &chareink_10_regular);
  m.registerFont(LATO_12_FONT_ID, 12, "lato", &lato_12RegularFont, &lato_12BoldFont, &lato_12ItalicFont,
                 &lato_12BoldItalicFont, &chareink_12_regular);
  m.registerFont(LATO_14_FONT_ID, 14, "lato", &lato_14RegularFont, &lato_14BoldFont, &lato_14ItalicFont,
                 &lato_14BoldItalicFont, &chareink_14_regular);
  m.registerFont(LATO_16_FONT_ID, 16, "lato", &lato_16RegularFont, &lato_16BoldFont, &lato_16ItalicFont,
                 &lato_16BoldItalicFont, &chareink_16_regular);
  m.registerFont(LATO_17_FONT_ID, 17, "lato", &lato_17RegularFont, &lato_17BoldFont, &lato_17ItalicFont,
                 &lato_17BoldItalicFont, &chareink_17_regular);

  m.registerFont(HELVETICA_10_FONT_ID, 10, "helvetica", &helvetica_10RegularFont, &helvetica_10BoldFont,
                 &helvetica_10ItalicFont, nullptr, &chareink_10_regular);
  m.registerFont(HELVETICA_12_FONT_ID, 12, "helvetica", &helvetica_12RegularFont, &helvetica_12BoldFont,
                 &helvetica_12ItalicFont, nullptr, &chareink_12_regular);
  m.registerFont(HELVETICA_14_FONT_ID, 14, "helvetica", &helvetica_14RegularFont, &helvetica_14BoldFont,
                 &helvetica_14ItalicFont, nullptr, &chareink_14_regular);
  m.registerFont(HELVETICA_16_FONT_ID, 16, "helvetica", &helvetica_16RegularFont, &helvetica_16BoldFont,
                 &helvetica_16ItalicFont, nullptr, &chareink_16_regular);
  m.registerFont(HELVETICA_17_FONT_ID, 17, "helvetica", &helvetica_17RegularFont, &helvetica_17BoldFont,
                 &helvetica_17ItalicFont, nullptr, &chareink_17_regular);

  m.registerFont(VERDANA_10_FONT_ID, 10, "verdana", &verdana_10RegularFont, &verdana_10BoldFont, &verdana_10ItalicFont,
                 nullptr, &chareink_10_regular);
  m.registerFont(VERDANA_12_FONT_ID, 12, "verdana", &verdana_12RegularFont, &verdana_12BoldFont, &verdana_12ItalicFont,
                 nullptr, &chareink_12_regular);
  m.registerFont(VERDANA_14_FONT_ID, 14, "verdana", &verdana_14RegularFont, &verdana_14BoldFont, &verdana_14ItalicFont,
                 nullptr, &chareink_14_regular);
  m.registerFont(VERDANA_16_FONT_ID, 16, "verdana", &verdana_16RegularFont, &verdana_16BoldFont, &verdana_16ItalicFont,
                 nullptr, &chareink_16_regular);
  m.registerFont(VERDANA_17_FONT_ID, 17, "verdana", &verdana_17RegularFont, &verdana_17BoldFont, &verdana_17ItalicFont,
                 nullptr, &chareink_17_regular);

  // SD-only Tier-1 extra sizes 11/13/15/18 (tables in flash, bitmaps streamed from
  // SD packs — same path as the existing sizes, zero added heap).
  m.registerFont(BOOKERLY_11_FONT_ID, 11, "bookerly", &bookerly_11RegularFont, &bookerly_11BoldFont,
                 &bookerly_11ItalicFont, nullptr, &chareink_12_regular);
  m.registerFont(BOOKERLY_13_FONT_ID, 13, "bookerly", &bookerly_13RegularFont, &bookerly_13BoldFont,
                 &bookerly_13ItalicFont, nullptr, &chareink_14_regular);
  m.registerFont(BOOKERLY_15_FONT_ID, 15, "bookerly", &bookerly_15RegularFont, &bookerly_15BoldFont,
                 &bookerly_15ItalicFont, nullptr, &chareink_16_regular);
  m.registerFont(BOOKERLY_18_FONT_ID, 18, "bookerly", &bookerly_18RegularFont, &bookerly_18BoldFont,
                 &bookerly_18ItalicFont, nullptr, &chareink_17_regular);
  m.registerFont(GEORGIA_11_FONT_ID, 11, "georgia", &georgia_11RegularFont, &georgia_11BoldFont, &georgia_11ItalicFont,
                 nullptr, &chareink_12_regular);
  m.registerFont(GEORGIA_13_FONT_ID, 13, "georgia", &georgia_13RegularFont, &georgia_13BoldFont, &georgia_13ItalicFont,
                 nullptr, &chareink_14_regular);
  m.registerFont(GEORGIA_15_FONT_ID, 15, "georgia", &georgia_15RegularFont, &georgia_15BoldFont, &georgia_15ItalicFont,
                 nullptr, &chareink_16_regular);
  m.registerFont(GEORGIA_18_FONT_ID, 18, "georgia", &georgia_18RegularFont, &georgia_18BoldFont, &georgia_18ItalicFont,
                 nullptr, &chareink_17_regular);
  m.registerFont(LATO_11_FONT_ID, 11, "lato", &lato_11RegularFont, &lato_11BoldFont, &lato_11ItalicFont,
                 &lato_11BoldItalicFont, &chareink_12_regular);
  m.registerFont(LATO_13_FONT_ID, 13, "lato", &lato_13RegularFont, &lato_13BoldFont, &lato_13ItalicFont,
                 &lato_13BoldItalicFont, &chareink_14_regular);
  m.registerFont(LATO_15_FONT_ID, 15, "lato", &lato_15RegularFont, &lato_15BoldFont, &lato_15ItalicFont,
                 &lato_15BoldItalicFont, &chareink_16_regular);
  m.registerFont(LATO_18_FONT_ID, 18, "lato", &lato_18RegularFont, &lato_18BoldFont, &lato_18ItalicFont,
                 &lato_18BoldItalicFont, &chareink_17_regular);
  m.registerFont(HELVETICA_11_FONT_ID, 11, "helvetica", &helvetica_11RegularFont, &helvetica_11BoldFont,
                 &helvetica_11ItalicFont, nullptr, &chareink_12_regular);
  m.registerFont(HELVETICA_13_FONT_ID, 13, "helvetica", &helvetica_13RegularFont, &helvetica_13BoldFont,
                 &helvetica_13ItalicFont, nullptr, &chareink_14_regular);
  m.registerFont(HELVETICA_15_FONT_ID, 15, "helvetica", &helvetica_15RegularFont, &helvetica_15BoldFont,
                 &helvetica_15ItalicFont, nullptr, &chareink_16_regular);
  m.registerFont(HELVETICA_18_FONT_ID, 18, "helvetica", &helvetica_18RegularFont, &helvetica_18BoldFont,
                 &helvetica_18ItalicFont, nullptr, &chareink_17_regular);
  m.registerFont(VERDANA_11_FONT_ID, 11, "verdana", &verdana_11RegularFont, &verdana_11BoldFont, &verdana_11ItalicFont,
                 nullptr, &chareink_12_regular);
  m.registerFont(VERDANA_13_FONT_ID, 13, "verdana", &verdana_13RegularFont, &verdana_13BoldFont, &verdana_13ItalicFont,
                 nullptr, &chareink_14_regular);
  m.registerFont(VERDANA_15_FONT_ID, 15, "verdana", &verdana_15RegularFont, &verdana_15BoldFont, &verdana_15ItalicFont,
                 nullptr, &chareink_16_regular);
  m.registerFont(VERDANA_18_FONT_ID, 18, "verdana", &verdana_18RegularFont, &verdana_18BoldFont, &verdana_18ItalicFont,
                 nullptr, &chareink_17_regular);
  m.registerFont(MERRIWEATHER_10_FONT_ID, 10, "merriweather", &merriweather_10RegularFont, &merriweather_10BoldFont,
                 &merriweather_10ItalicFont, &merriweather_10BoldItalicFont, &chareink_10_regular);
  m.registerFont(MERRIWEATHER_11_FONT_ID, 11, "merriweather", &merriweather_11RegularFont, &merriweather_11BoldFont,
                 &merriweather_11ItalicFont, &merriweather_11BoldItalicFont, &chareink_12_regular);
  m.registerFont(MERRIWEATHER_12_FONT_ID, 12, "merriweather", &merriweather_12RegularFont, &merriweather_12BoldFont,
                 &merriweather_12ItalicFont, &merriweather_12BoldItalicFont, &chareink_12_regular);
  m.registerFont(MERRIWEATHER_13_FONT_ID, 13, "merriweather", &merriweather_13RegularFont, &merriweather_13BoldFont,
                 &merriweather_13ItalicFont, &merriweather_13BoldItalicFont, &chareink_14_regular);
  m.registerFont(MERRIWEATHER_14_FONT_ID, 14, "merriweather", &merriweather_14RegularFont, &merriweather_14BoldFont,
                 &merriweather_14ItalicFont, &merriweather_14BoldItalicFont, &chareink_14_regular);
  m.registerFont(MERRIWEATHER_15_FONT_ID, 15, "merriweather", &merriweather_15RegularFont, &merriweather_15BoldFont,
                 &merriweather_15ItalicFont, &merriweather_15BoldItalicFont, &chareink_16_regular);
  m.registerFont(MERRIWEATHER_16_FONT_ID, 16, "merriweather", &merriweather_16RegularFont, &merriweather_16BoldFont,
                 &merriweather_16ItalicFont, &merriweather_16BoldItalicFont, &chareink_16_regular);
  m.registerFont(MERRIWEATHER_17_FONT_ID, 17, "merriweather", &merriweather_17RegularFont, &merriweather_17BoldFont,
                 &merriweather_17ItalicFont, &merriweather_17BoldItalicFont, &chareink_17_regular);
  m.registerFont(MERRIWEATHER_18_FONT_ID, 18, "merriweather", &merriweather_18RegularFont, &merriweather_18BoldFont,
                 &merriweather_18ItalicFont, &merriweather_18BoldItalicFont, &chareink_17_regular);
  m.registerFont(PLAYFAIR_10_FONT_ID, 10, "playfair", &playfair_10RegularFont, &playfair_10BoldFont,
                 &playfair_10ItalicFont, &playfair_10BoldItalicFont, &chareink_10_regular);
  m.registerFont(PLAYFAIR_11_FONT_ID, 11, "playfair", &playfair_11RegularFont, &playfair_11BoldFont,
                 &playfair_11ItalicFont, &playfair_11BoldItalicFont, &chareink_12_regular);
  m.registerFont(PLAYFAIR_12_FONT_ID, 12, "playfair", &playfair_12RegularFont, &playfair_12BoldFont,
                 &playfair_12ItalicFont, &playfair_12BoldItalicFont, &chareink_12_regular);
  m.registerFont(PLAYFAIR_13_FONT_ID, 13, "playfair", &playfair_13RegularFont, &playfair_13BoldFont,
                 &playfair_13ItalicFont, &playfair_13BoldItalicFont, &chareink_14_regular);
  m.registerFont(PLAYFAIR_14_FONT_ID, 14, "playfair", &playfair_14RegularFont, &playfair_14BoldFont,
                 &playfair_14ItalicFont, &playfair_14BoldItalicFont, &chareink_14_regular);
  m.registerFont(PLAYFAIR_15_FONT_ID, 15, "playfair", &playfair_15RegularFont, &playfair_15BoldFont,
                 &playfair_15ItalicFont, &playfair_15BoldItalicFont, &chareink_16_regular);
  m.registerFont(PLAYFAIR_16_FONT_ID, 16, "playfair", &playfair_16RegularFont, &playfair_16BoldFont,
                 &playfair_16ItalicFont, &playfair_16BoldItalicFont, &chareink_16_regular);
  m.registerFont(PLAYFAIR_17_FONT_ID, 17, "playfair", &playfair_17RegularFont, &playfair_17BoldFont,
                 &playfair_17ItalicFont, &playfair_17BoldItalicFont, &chareink_17_regular);
  m.registerFont(PLAYFAIR_18_FONT_ID, 18, "playfair", &playfair_18RegularFont, &playfair_18BoldFont,
                 &playfair_18ItalicFont, &playfair_18BoldItalicFont, &chareink_17_regular);
  m.registerFont(GALMURI_14_FONT_ID, 14, "galmuri", &galmuri_14RegularFont, &galmuri_14BoldFont, &galmuri_14ItalicFont,
                 nullptr, &chareink_14_regular);
  m.registerFont(GALMURI_28_FONT_ID, 28, "galmuri", &galmuri_28RegularFont, &galmuri_28BoldFont, &galmuri_28ItalicFont,
                 nullptr, &chareink_17_regular);
  m.registerFont(VOLLKORN_10_FONT_ID, 10, "vollkorn", &vollkorn_10RegularFont, &vollkorn_10BoldFont,
                 &vollkorn_10ItalicFont, &vollkorn_10BoldItalicFont, &chareink_10_regular);
  m.registerFont(VOLLKORN_11_FONT_ID, 11, "vollkorn", &vollkorn_11RegularFont, &vollkorn_11BoldFont,
                 &vollkorn_11ItalicFont, &vollkorn_11BoldItalicFont, &chareink_12_regular);
  m.registerFont(VOLLKORN_12_FONT_ID, 12, "vollkorn", &vollkorn_12RegularFont, &vollkorn_12BoldFont,
                 &vollkorn_12ItalicFont, &vollkorn_12BoldItalicFont, &chareink_12_regular);
  m.registerFont(VOLLKORN_13_FONT_ID, 13, "vollkorn", &vollkorn_13RegularFont, &vollkorn_13BoldFont,
                 &vollkorn_13ItalicFont, &vollkorn_13BoldItalicFont, &chareink_14_regular);
  m.registerFont(VOLLKORN_14_FONT_ID, 14, "vollkorn", &vollkorn_14RegularFont, &vollkorn_14BoldFont,
                 &vollkorn_14ItalicFont, &vollkorn_14BoldItalicFont, &chareink_14_regular);
  m.registerFont(VOLLKORN_15_FONT_ID, 15, "vollkorn", &vollkorn_15RegularFont, &vollkorn_15BoldFont,
                 &vollkorn_15ItalicFont, &vollkorn_15BoldItalicFont, &chareink_16_regular);
  m.registerFont(VOLLKORN_16_FONT_ID, 16, "vollkorn", &vollkorn_16RegularFont, &vollkorn_16BoldFont,
                 &vollkorn_16ItalicFont, &vollkorn_16BoldItalicFont, &chareink_16_regular);
  m.registerFont(VOLLKORN_17_FONT_ID, 17, "vollkorn", &vollkorn_17RegularFont, &vollkorn_17BoldFont,
                 &vollkorn_17ItalicFont, &vollkorn_17BoldItalicFont, &chareink_17_regular);
  m.registerFont(VOLLKORN_18_FONT_ID, 18, "vollkorn", &vollkorn_18RegularFont, &vollkorn_18BoldFont,
                 &vollkorn_18ItalicFont, &vollkorn_18BoldItalicFont, &chareink_17_regular);
}
#endif  // CROSSPOINT_SD_FONTS

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  LOG_DBG("MAIN", "Display initialized");
  // ChareInk removed (Lector).
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
  renderer.insertFont(BOOKERLY_10_FONT_ID, bookerly10FontFamily);
#endif
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_17_FONT_ID, bookerly17FontFamily);
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
  renderer.insertFont(GEORGIA_10_FONT_ID, georgia_10FontFamily);
#endif
  renderer.insertFont(GEORGIA_12_FONT_ID, georgia_12FontFamily);
  renderer.insertFont(GEORGIA_14_FONT_ID, georgia_14FontFamily);
  renderer.insertFont(GEORGIA_16_FONT_ID, georgia_16FontFamily);
  renderer.insertFont(GEORGIA_17_FONT_ID, georgia_17FontFamily);
  // Lato removed (Lector).
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
  renderer.insertFont(HELVETICA_10_FONT_ID, helvetica_10FontFamily);
#endif
  renderer.insertFont(HELVETICA_12_FONT_ID, helvetica_12FontFamily);
  renderer.insertFont(HELVETICA_14_FONT_ID, helvetica_14FontFamily);
  renderer.insertFont(HELVETICA_16_FONT_ID, helvetica_16FontFamily);
  renderer.insertFont(HELVETICA_17_FONT_ID, helvetica_17FontFamily);
#if !defined(CROSSPOINT_FLASH_EXTRA_SIZES)  // size 10 dropped from flash-extra picker
  renderer.insertFont(VERDANA_10_FONT_ID, verdana_10FontFamily);
#endif
  renderer.insertFont(VERDANA_12_FONT_ID, verdana_12FontFamily);
  renderer.insertFont(VERDANA_14_FONT_ID, verdana_14FontFamily);
  renderer.insertFont(VERDANA_16_FONT_ID, verdana_16FontFamily);
  renderer.insertFont(VERDANA_17_FONT_ID, verdana_17FontFamily);
#if defined(CROSSPOINT_SD_FONTS) || defined(CROSSPOINT_FLASH_EXTRA_SIZES)
  // Sizes 11/13/15 of the 5 flash families — registered in the flash-extra build.
  renderer.insertFont(BOOKERLY_11_FONT_ID, bookerly_11FontFamily);
  renderer.insertFont(BOOKERLY_13_FONT_ID, bookerly_13FontFamily);
  renderer.insertFont(BOOKERLY_15_FONT_ID, bookerly_15FontFamily);
  renderer.insertFont(GEORGIA_11_FONT_ID, georgia_11FontFamily);
  renderer.insertFont(GEORGIA_13_FONT_ID, georgia_13FontFamily);
  renderer.insertFont(GEORGIA_15_FONT_ID, georgia_15FontFamily);
  renderer.insertFont(HELVETICA_11_FONT_ID, helvetica_11FontFamily);
  renderer.insertFont(HELVETICA_13_FONT_ID, helvetica_13FontFamily);
  renderer.insertFont(HELVETICA_15_FONT_ID, helvetica_15FontFamily);
  renderer.insertFont(VERDANA_11_FONT_ID, verdana_11FontFamily);
  renderer.insertFont(VERDANA_13_FONT_ID, verdana_13FontFamily);
  renderer.insertFont(VERDANA_15_FONT_ID, verdana_15FontFamily);
#endif  // flash extra sizes 11/13/15
#if defined(CROSSPOINT_FLASH_EXTRA_SIZES) && !defined(CROSSPOINT_SD_FONTS)
  // Lector: Merriweather (5th flash family), sizes 11..17.
  renderer.insertFont(MERRIWEATHER_11_FONT_ID, merriweather_11FontFamily);
  renderer.insertFont(MERRIWEATHER_12_FONT_ID, merriweather_12FontFamily);
  renderer.insertFont(MERRIWEATHER_13_FONT_ID, merriweather_13FontFamily);
  renderer.insertFont(MERRIWEATHER_14_FONT_ID, merriweather_14FontFamily);
  renderer.insertFont(MERRIWEATHER_15_FONT_ID, merriweather_15FontFamily);
  renderer.insertFont(MERRIWEATHER_16_FONT_ID, merriweather_16FontFamily);
  renderer.insertFont(MERRIWEATHER_17_FONT_ID, merriweather_17FontFamily);
#endif  // flash Merriweather 11..17
#ifdef CROSSPOINT_SD_FONTS
  // Size 18 of the 5 flash families + the SD-only families — SD builds only.
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly_18FontFamily);
  renderer.insertFont(GEORGIA_18_FONT_ID, georgia_18FontFamily);
  renderer.insertFont(LATO_18_FONT_ID, lato_18FontFamily);
  renderer.insertFont(HELVETICA_18_FONT_ID, helvetica_18FontFamily);
  renderer.insertFont(VERDANA_18_FONT_ID, verdana_18FontFamily);
  renderer.insertFont(MERRIWEATHER_10_FONT_ID, merriweather_10FontFamily);
  renderer.insertFont(MERRIWEATHER_11_FONT_ID, merriweather_11FontFamily);
  renderer.insertFont(MERRIWEATHER_12_FONT_ID, merriweather_12FontFamily);
  renderer.insertFont(MERRIWEATHER_13_FONT_ID, merriweather_13FontFamily);
  renderer.insertFont(MERRIWEATHER_14_FONT_ID, merriweather_14FontFamily);
  renderer.insertFont(MERRIWEATHER_15_FONT_ID, merriweather_15FontFamily);
  renderer.insertFont(MERRIWEATHER_16_FONT_ID, merriweather_16FontFamily);
  renderer.insertFont(MERRIWEATHER_17_FONT_ID, merriweather_17FontFamily);
  renderer.insertFont(MERRIWEATHER_18_FONT_ID, merriweather_18FontFamily);
  renderer.insertFont(PLAYFAIR_10_FONT_ID, playfair_10FontFamily);
  renderer.insertFont(PLAYFAIR_11_FONT_ID, playfair_11FontFamily);
  renderer.insertFont(PLAYFAIR_12_FONT_ID, playfair_12FontFamily);
  renderer.insertFont(PLAYFAIR_13_FONT_ID, playfair_13FontFamily);
  renderer.insertFont(PLAYFAIR_14_FONT_ID, playfair_14FontFamily);
  renderer.insertFont(PLAYFAIR_15_FONT_ID, playfair_15FontFamily);
  renderer.insertFont(PLAYFAIR_16_FONT_ID, playfair_16FontFamily);
  renderer.insertFont(PLAYFAIR_17_FONT_ID, playfair_17FontFamily);
  renderer.insertFont(PLAYFAIR_18_FONT_ID, playfair_18FontFamily);
  renderer.insertFont(GALMURI_14_FONT_ID, galmuri_14FontFamily);
  renderer.insertFont(GALMURI_28_FONT_ID, galmuri_28FontFamily);
  renderer.insertFont(VOLLKORN_10_FONT_ID, vollkorn_10FontFamily);
  renderer.insertFont(VOLLKORN_11_FONT_ID, vollkorn_11FontFamily);
  renderer.insertFont(VOLLKORN_12_FONT_ID, vollkorn_12FontFamily);
  renderer.insertFont(VOLLKORN_13_FONT_ID, vollkorn_13FontFamily);
  renderer.insertFont(VOLLKORN_14_FONT_ID, vollkorn_14FontFamily);
  renderer.insertFont(VOLLKORN_15_FONT_ID, vollkorn_15FontFamily);
  renderer.insertFont(VOLLKORN_16_FONT_ID, vollkorn_16FontFamily);
  renderer.insertFont(VOLLKORN_17_FONT_ID, vollkorn_17FontFamily);
  renderer.insertFont(VOLLKORN_18_FONT_ID, vollkorn_18FontFamily);
#endif  // CROSSPOINT_SD_FONTS
  // Cozette UI (v11.0.0 sizes): SMALL = ui_8 status bar, UI_10 = ui_10 body/menus,
  // UI_12 = ui_12 titles (distinct size, not aliased).
  renderer.insertFont(UI_10_FONT_ID, uiFontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  static FontCacheManager fontCacheManager(renderer.getFontMap());
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  // Register the font-cache as the SafeAnywhere shed evictor now that FCM is
  // reachable via renderer, so the failed-alloc callback and mem::roomToGrow
  // both drop it under pressure (RFC #163). Idempotent across re-entry only if
  // setup runs once, which it does.
  crosspoint::mem::registerShedEvictor(&shedFontCacheEvictor);

  // Register the heap allocator failure hook now that FCM is reachable
  // via renderer. Idempotent: a re-call replaces the previous handler.
  // ESP-IDF's documented contract — runs on the failing task, retries
  // the allocation once after we return — is what makes this useful as
  // a defragmentation trigger rather than just a logging hook.
  if (heap_caps_register_failed_alloc_callback(&onHeapAllocFailed) != ESP_OK) {
    LOG_ERR("MAIN", "heap_caps_register_failed_alloc_callback failed");
  }
  // Install the C++ new-handler: the guaranteed-retry shed-and-recover net for
  // operator new / STL growth (the C-level callback above covers raw malloc).
  crosspoint::mem::installOomHandler();
  LOG_DBG("MAIN", "Fonts setup");
}

bool ensureCrosspointDataDir() {
  constexpr const char* dataDir = Paths::kDataDir;

  if (Storage.exists(dataDir)) {
    FsFile entry = Storage.open(dataDir, O_RDONLY);
    if (!entry) {
      LOG_ERR("MAIN", "Failed to inspect %s", dataDir);
      return false;
    }

    const bool isDirectory = entry.isDirectory();
    entry.close();
    if (!isDirectory) {
      constexpr const char* quarantinePath = Paths::kCorruptQuarantine;
      if (Storage.exists(quarantinePath)) {
        if (!Storage.remove(quarantinePath)) {
          Storage.removeDir(quarantinePath);
        }
      }

      if (!Storage.rename(dataDir, quarantinePath)) {
        LOG_ERR("MAIN", "Failed to quarantine invalid %s", dataDir);
        if (!Storage.remove(dataDir)) {
          return false;
        }
      } else {
        LOG_ERR("MAIN", "Quarantined invalid %s to %s", dataDir, quarantinePath);
      }
    }
  }

  if (!Storage.mkdir(dataDir)) {
    FsFile dir = Storage.open(dataDir, O_RDONLY);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      LOG_ERR("MAIN", "Failed to ensure %s directory", dataDir);
      return false;
    }
    dir.close();
  }

  return true;
}

// Boot heap-stage probe. Originally PR #104 investigation; now gated so the
// 9 probe sites compile to no-ops in production builds. Re-enable with
// -DENABLE_BOOT_HEAP_TRACE when investigating fragmentation regressions.
// Rationale for the gate: unconditional probes crowd the 16-line RTC log
// ring with stale boot data, evicting the more useful pre-panic context.
#ifdef ENABLE_BOOT_HEAP_TRACE
static void logHeapStage(const char* label) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  LOG_DIAG("BOOT", "stage='%s' free=%u largest=%u free_blocks=%u alloc_blocks=%u total=%u", label,
           (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block, (unsigned)info.free_blocks,
           (unsigned)info.allocated_blocks, (unsigned)(info.total_free_bytes + info.total_allocated_bytes));
}
#else
static inline void logHeapStage(const char*) {}
#endif

void setup() {
  t1 = millis();

  gpio.begin();
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  powerManager.begin();
  HalSystem::begin();

  // X3-only peripherals. Both self-gate on gpio.deviceIsX3() and share the I2C
  // bus HalPowerManager::begin() just opened, so on an X4 these leave the HALs
  // unavailable and every clock/tilt code path downstream is inert.
  halClock.begin();
  halTiltSensor.begin();

  // Silent-reboot detection (PR upstream #1908). Run-and-clear right after
  // HalSystem::begin so a panic later in setup() can't loop us back into a
  // silent reboot. -1 = normal boot, 0 = home target, 1 = reader target.
  // The flag survives ESP.restart() via RTC_NOINIT memory but is wiped on
  // hard power loss; cold-boot magic mismatch reads as normal boot.
  const int silentRebootTarget = consumeSilentRebootTarget();
  const bool isSilentReboot = (silentRebootTarget >= 0);
  if (isSilentReboot) {
    // LOG_DIAG so the resume event survives in the RTC ring across boots.
    LOG_DIAG("MAIN", "Silent reboot detected (target=%d) — skipping boot splash", silentRebootTarget);
  }

  // Only start serial if USB connected
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    // Wait up to 500ms for Serial to be ready (enough for USB CDC enumeration)
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 500) {
      delay(10);
    }
  }
  logHeapStage("after_serial");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    exitActivity();
    enterNewActivity(new (std::nothrow) FullScreenMessageActivity(renderer, mappedInputManager, "SD card error",
                                                                  EpdFontFamily::REGULAR));
    return;
  }

  // Background SD-write task. Must come up after Storage.begin() so submitted
  // jobs can use HalStorage; lifecycle drains in persistAppState() and force-
  // flush callers ensure no in-flight write outlives a deep sleep.
  crosspoint::persist::AsyncWriter::instance().start();

  // Dump crash report to SD card if we rebooted from a panic
  HalSystem::checkPanic();
  HalSystem::clearPanic();

  // Arm the Wi-Fi diagnostic reporter — registers the WiFi.onEvent handler
  // so disconnect reason codes are captured for /wifi_report.txt on failure.
  WifiDiagReport::begin();

  if (!ensureCrosspointDataDir()) {
    LOG_ERR("MAIN", "Storage layout error: cannot access /.crosspoint");
  }

  // One-shot boot cleanup of orphaned safeWriteFile temp files.
  // Background: settings.json.tmp from an interrupted prior write occasionally
  // ends up in a state where Storage.remove() returns false on every save, so
  // safeWriteFile falls through to settings.json.tmp2 and logs an ERR each
  // time. Try aggressive removal at boot when no save is in flight: remove,
  // then truncate-via-openForWrite + close + remove, then last-resort rename
  // to a junk path so a future boot can sweep it.
  auto sweepOrphanTmp = [](const char* primaryPath) {
    char tmpPath[128];
    char tmp2Path[128];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", primaryPath);
    snprintf(tmp2Path, sizeof(tmp2Path), "%s.tmp2", primaryPath);
    for (const char* p : {static_cast<const char*>(tmpPath), static_cast<const char*>(tmp2Path)}) {
      if (!Storage.exists(p)) continue;
      if (Storage.remove(p)) {
        LOG_INF("MAIN", "Boot tmp sweep: removed orphan %s", p);
        continue;
      }
      // Truncate-then-remove. Reopening for write zero-pads the entry; on SdFat
      // this often unsticks a directory entry that plain remove() fails on.
      FsFile f;
      if (Storage.openFileForWrite("MAIN", p, f)) {
        f.close();
        if (Storage.remove(p)) {
          LOG_INF("MAIN", "Boot tmp sweep: removed orphan %s after truncate", p);
          continue;
        }
      }
      // Last resort — rename to a junk path with a unique suffix so it stops
      // being mistaken for a live tmp by safeWriteFile. Future boots can try
      // again on the junk path.
      char junkPath[160];
      snprintf(junkPath, sizeof(junkPath), "%s.junk-%lu", p, (unsigned long)millis());
      if (Storage.rename(p, junkPath)) {
        LOG_INF("MAIN", "Boot tmp sweep: renamed stuck %s -> %s", p, junkPath);
      } else {
        LOG_ERR("MAIN", "Boot tmp sweep: %s remains stuck (remove + truncate + rename all failed)", p);
      }
    }
  };
  sweepOrphanTmp("/.crosspoint/settings.json");
  sweepOrphanTmp("/.crosspoint/state.json");
  sweepOrphanTmp("/.crosspoint/recent.json");
  sweepOrphanTmp("/.crosspoint/themes.json");

  logHeapStage("after_sd");
  trash::pruneToCap();
  logHeapStage("after_trash_prune");

  SETTINGS.loadFromFile();
  logHeapStage("after_settings_load");
  setBitmapHelpersUseFactoryLUT(SETTINGS.useFactoryLUT != 0);
  I18N.setLanguage(static_cast<Language>(SETTINGS.uiLanguage));
  // Retry theme load up to 3 times — a transient SD read failure here would
  // leave the theme list empty, and any later save could overwrite the file.
  for (int attempt = 0; attempt < 3; attempt++) {
    if (READING_THEMES.loadFromFile()) {
      break;
    }
    LOG_ERR("MAIN", "Theme load attempt %d failed, retrying...", attempt + 1);
    delay(50);
  }
  logHeapStage("after_themes_load");
  KOREADER_STORE.loadFromFile();
  logHeapStage("after_koreader_load");
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      // For normal wakeups, verify power button press duration
      LOG_DBG("MAIN", "Verifying power button press duration");
      verifyPowerButtonDuration();
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint-Mod-DX34 version " CROSSPOINT_VERSION);

  setupDisplayAndFonts();
  logHeapStage("after_display_fonts");

#ifdef CROSSPOINT_SD_FONTS
  // SD-backed reader fonts. SD is mounted (Storage.begin above) and every family
  // is registered with the renderer (pointer-based, so the manager repointing an
  // EpdFont propagates into the already-inserted family). On a fat/export build
  // exportAllMissing() writes any missing /fonts/*.bin from the in-flash bitmaps;
  // on a slim build those bitmaps are gone and the glyphs stream from the packs.
  // The active reader font is backed immediately; renderContents()/renderPage()
  // re-affirm whenever the selection changes.
  static crosspoint::fonts::HalSdFontIo g_sdFontIo;
  crosspoint::fonts::sdFonts().setIo(&g_sdFontIo);
  registerSdReaderFonts();
  Storage.mkdir("/fonts");  // ensure the dir exists so the browser pack-upload lands (slim never exports)
  crosspoint::fonts::sdFonts().exportAllMissing();
  crosspoint::fonts::activateReaderFont(SETTINGS.getReaderFontId());
#endif

  exitActivity();
  // Silent-reboot path skips the splash entirely — the screen state from the
  // pre-reboot session is still on the e-ink panel, so a fresh splash would
  // be a jarring flash. ActivityRouter::begin below will render the
  // destination directly.
  BootActivity* bootActivity = nullptr;
  if (!isSilentReboot) {
    bootActivity = new (std::nothrow) BootActivity(renderer, mappedInputManager);
    enterNewActivity(bootActivity);  // null-safe: routes to OOM recovery
    if (bootActivity) bootActivity->setProgress(32, "Restoring state");
  }
  {
    // Touch the store so it registers with PersistManager before the sidecar
    // backup runs (backup iterates registered store paths).
    (void)crosspoint::persist::appStateStore();
    crosspoint::persist::SdFatFileIO sidecarIo;
    if (crosspoint::persist::PersistManager().backupSidecarIfNewFirmware(sidecarIo, CROSSPOINT_VERSION)) {
      LOG_INF("MAIN", "First boot of %s — SD sidecar backup written", CROSSPOINT_VERSION);
    }
  }
  // Guarantee every silent restart flushes pending durable state before
  // rebooting: most silent-restart call sites (WiFi-session exits, OOM-on-entry)
  // don't flush themselves, so a debounced write queued just before the reboot
  // would be lost. One boot-time hook makes the flush structural.
  registerPreRestartHook([] { (void)crosspoint::persist::PersistManager().flushAll(); });

  APP_STATE.loadFromFile();
  logHeapStage("after_app_state");

  // wallpaper:: facade lazy-wires production deps on first call (e.g.
  // markFolderDirty during file transfer, advance() inside SleepActivity).
  // No explicit setup call needed here.

  LOG_INF("MAIN", "Booting complete, checking initial activity");

  // Always load recents early — reader activities call addBook() which saves
  // to disk, so an unloaded list would overwrite the file with just one entry.
  RECENT_BOOKS.loadFromFile();
  logHeapStage("after_recents");

  std::string readerPath;
  bool goHome;
  {
    // RFC #166: the boot-destination decision — including the brick-class
    // crash-loop guard (readerActivityLoadCount) — now lives in the pure,
    // host-tested crosspoint::boot::decideBoot. Gather the impure inputs here,
    // let the core decide, then apply the durable guard side effect it asks
    // for. The decision itself is a byte-identical transcription of the old
    // inline logic; the random fn receives the FILTERED epub count from inside
    // the core, so we never re-derive the .epub filter at the call site.
    crosspoint::boot::BootInputs in;
    in.isSilentReboot = isSilentReboot;
    in.silentRebootTarget = silentRebootTarget;
    in.openEpubPath = APP_STATE.openEpubPath;
    in.readerActivityLoadCount = APP_STATE.readerActivityLoadCount;
    in.backHeld = mappedInputManager.isPressed(MappedInputManager::Button::Back);
    in.randomBookOnBoot = SETTINGS.randomBookOnBoot;
    for (const auto& b : RECENT_BOOKS.getBooks()) in.recentBookPaths.push_back(b.path);

    const crosspoint::boot::BootDecision decision = crosspoint::boot::decideBoot(
        in, [](uint32_t count) -> uint32_t { return static_cast<uint32_t>(random(static_cast<long>(count))); });
    readerPath = decision.readerPath;
    goHome = decision.goHome;

    if (decision.bumpGuard) {
      APP_STATE.openEpubPath = "";
      if (APP_STATE.readerActivityLoadCount < 255) APP_STATE.readerActivityLoadCount++;
      // Crash-loop guard must be DURABLE before we launch the reader: a book
      // whose open OOMs (reader-render-oom) restarts within setup() before the
      // first loop() tick, so a debounced saveToFile() would never drain and
      // the guard increment would be lost — a power-cycle then re-reads count 0
      // and re-opens the same poisoned book forever. Flush synchronously so the
      // very next boot sees count > 0 → forcedHome → library. (The render path
      // clears this latch on the first successful render and preserves it on an
      // OOM give-up; see EpubReaderActivity.)
      APP_STATE.saveToFileSync();
    }
  }

  // Defer sleep cache trimming until home screen is actually needed.
  if (goHome) {
    if (bootActivity) bootActivity->setProgress(60, "Refreshing sleep cache");
    // Boot reconcile: ensureLoaded would safeRead a multi-KB sleep_order.txt
    // as a single std::string, which the boot heap cannot guarantee. Mark
    // dirty only; first sleep entry consumes it under the rich-sleep
    // heap-budget gate.
    crosspoint::sleep::wallpaper::markFolderDirty();
  }
  if (bootActivity) bootActivity->setProgress(80, goHome ? "Preparing home" : "Resuming book");

  wireActivityRouter();

  if (bootActivity) bootActivity->setProgress(100, goHome ? "Opening home" : "Opening book");

  auto launchBootDestination = [goHome, readerPath]() {
    if (goHome) {
      lifecycle::ActivityRouter::instance().begin({lifecycle::RouteId::Home, ""});
    } else {
      lifecycle::ActivityRouter::instance().begin({lifecycle::RouteId::Reader, readerPath});
    }
  };

  launchBootDestination();

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

  // X3 gyro tilt page-turn. On an X4 the sensor is unavailable so this returns
  // on the first branch (no I2C, negligible cost). On an X3 it only polls the
  // IMU while in the reader with tilt enabled; events are consumed by the
  // reader's input snapshot. Placed AFTER gpio.update() so it never delays input.
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation,
                       currentActivity && currentActivity->isReaderActivity());

  // Re-arm the OOM new-handler net and reset its per-episode shed budget, so a
  // memory-pressure episode in one tick gets a fresh shed-and-retry next tick.
  crosspoint::mem::installOomHandler();

  // Drain any coalesced dirty writes. Stores flush only when debounce
  // window has elapsed since the last markDirty; most ticks are no-ops.
  crosspoint::persist::PersistManager().tick(static_cast<uint32_t>(loopStartTime));

  // Only update fading fix when it changes (avoid calling every loop iteration)
  {
    static bool lastFadingFix = !SETTINGS.fadingFix;  // Force first-time update
    if (SETTINGS.fadingFix != lastFadingFix) {
      lastFadingFix = SETTINGS.fadingFix;
      renderer.setFadingFix(lastFadingFix);
    }
  }

  // Sync dark mode inversion flag with renderer when setting changes
  {
    static uint8_t lastDarkMode = !SETTINGS.darkMode;  // Force first-time update
    if (SETTINGS.darkMode != lastDarkMode) {
      lastDarkMode = SETTINGS.darkMode;
      renderer.setDarkMode(lastDarkMode != 0);
    }
  }

  if (Serial && millis() - lastMemPrint >= 10000) {
    // uxTaskGetStackHighWaterMark(nullptr) = minimum free stack (in words) the
    // loop task has ever had. A small/shrinking value warns of a stack-overflow
    // reset before it happens; logged alongside heap so both are visible.
    const unsigned loopStackFreeBytes = uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, Loop stack free: %u bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), loopStackFreeBytes);
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        logSerial.printf("SCREENSHOT_START:%d\n", HalDisplay::BUFFER_SIZE);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, HalDisplay::BUFFER_SIZE);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release), a button currently
  // held down (e.g. hold-to-scroll), or active background work. isAnyPressed()
  // keeps the CPU at full clock for the entire duration of a held button so the
  // idle power-saving throttle never engages mid-press and makes navigation
  // feel sluggish.
  static unsigned long lastActivityTime = millis();
  // A tilt-page-turn flick counts as activity so reading by tilt alone does not
  // auto-sleep. hadActivity() is consuming + always false on X4, so it is
  // evaluated unconditionally (not short-circuited) to keep the consume stable.
  const bool tiltActivity = halTiltSensor.hadActivity();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.isAnyPressed() || tiltActivity ||
      (currentActivity && currentActivity->preventAutoSleep())) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  auto triggerDeepSleep = []() {
    // Shown on both auto-sleep (inactivity timeout) and power-button-hold paths
    // so the device never blanks silently. renderer is a file-scope global.
    //
    // Tear down the current activity FIRST so its background render task
    // can't race the popup. Without this, ActivityWithSubactivity-derived
    // activities (Reader) keep painting the shared framebuffer between
    // show() and SleepActivity::onEnter: the render task fires a paint
    // that overwrites the popup pixels in BW and calls displayBuffer(),
    // which is forced to HALF_REFRESH by the pendingHalfRefresh flag set
    // by show(). The popup is wiped before the user perceives it.
    //
    // Capture fromReader BEFORE exitActivity destroys the slot; pass to
    // enterDeepSleep. enterDeepSleep's slot->onExit guard already handles
    // a nullptr slot (test_enter_deep_sleep_sequence_not_from_reader
    // exercises that path). The first persistAppState inside enterDeepSleep
    // still drains any async writes queued during onExit, so progress
    // durability is preserved.
    const bool fromReader = currentActivity && currentActivity->isReaderActivity();
    exitActivity();
    TransitionFeedback::resetStacking();
    TransitionFeedback::show(renderer, tr(STR_GOING_TO_SLEEP));
    TransitionFeedback::ensureMinDisplayElapsed();
    lifecycle::ActivityRouter::instance().enterDeepSleep(fromReader);
  };

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    triggerDeepSleep();
    // This should never be hit as enterDeepSleep calls esp_deep_sleep_start
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    triggerDeepSleep();
    // This should never be hit as enterDeepSleep calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (currentActivity) {
    currentActivity->loop();
  }

  // Drain any transition requested during currentActivity->loop() at a safe
  // boundary (after the activity has returned from its tick).
  lifecycle::ActivityRouter::instance().applyIfPending();

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms", maxLoopDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (currentActivity && currentActivity->skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power.
      // 20ms (was 50): caps the worst-case button-sample latency on the first
      // press after a reading pause to ~20ms instead of ~50ms, so a page-turn
      // after the 3s idle window no longer feels laggy. Still far cheaper than
      // the hot 2ms active loop.
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(20);
    } else {
      // Short delay to prevent tight loop while still being responsive. 2ms
      // (was 10ms, then 5ms) further trims worst-case button-detection latency
      // during active reading/navigation; cost is a hotter loop while the user
      // is actively pressing, which the power-saving branch above still reins
      // in after IDLE_POWER_SAVING_MS of inactivity.
      delay(2);
    }
  }
}
