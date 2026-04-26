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
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <cstring>
#include <new>

#include "Battery.h"
#include "BleHidManager.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "Paths.h"
#include "ReadingThemeStore.h"
#include "RecentBooksStore.h"
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
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "fonts/CustomBinFontIds.h"
#include "fonts/CustomBinFontManager.h"
#include "lifecycle/ActivityRouter.h"
#include "persist/AppStateStore.h"
#include "persist/PersistManager.h"
#include "persist/SdFatFileIO.h"
#include "persist/Trash.h"
#include "sleep/SdFatSleepFs.h"
#include "sleep/WallpaperPlaylist.h"
#if FEATURE_WALLPAPER_V2
#include "sleep/WallpaperPlaylistV2.h"
#endif
#include "util/ButtonNavigator.h"
#include "util/FavoriteBmp.h"
#include "util/TransitionFeedback.h"

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
FontDecompressor fontDecompressor;
Activity* currentActivity = nullptr;

// Fonts
EpdFont chareink12RegularFont(&chareink_12_regular);
EpdFont chareink12BoldFont(&chareink_12_bold);
EpdFont chareink12ItalicFont(&chareink_12_italic);
EpdFontFamily chareink12FontFamily(&chareink12RegularFont, &chareink12BoldFont, &chareink12ItalicFont, nullptr);
EpdFont chareink13RegularFont(&chareink_13_regular);
EpdFont chareink13BoldFont(&chareink_13_bold);
EpdFont chareink13ItalicFont(&chareink_13_italic);
EpdFontFamily chareink13FontFamily(&chareink13RegularFont, &chareink13BoldFont, &chareink13ItalicFont, nullptr);
EpdFont chareink14RegularFont(&chareink_14_regular);
EpdFont chareink14BoldFont(&chareink_14_bold);
EpdFont chareink14ItalicFont(&chareink_14_italic);
EpdFontFamily chareink14FontFamily(&chareink14RegularFont, &chareink14BoldFont, &chareink14ItalicFont, nullptr);
EpdFont chareink15RegularFont(&chareink_15_regular);
EpdFont chareink15BoldFont(&chareink_15_bold);
EpdFont chareink15ItalicFont(&chareink_15_italic);
EpdFontFamily chareink15FontFamily(&chareink15RegularFont, &chareink15BoldFont, &chareink15ItalicFont, nullptr);
EpdFont chareink16RegularFont(&chareink_16_regular);
EpdFont chareink16BoldFont(&chareink_16_bold);
EpdFont chareink16ItalicFont(&chareink_16_italic);
EpdFontFamily chareink16FontFamily(&chareink16RegularFont, &chareink16BoldFont, &chareink16ItalicFont, nullptr);
EpdFont chareink17RegularFont(&chareink_17_regular);
EpdFont chareink17BoldFont(&chareink_17_bold);
EpdFont chareink17ItalicFont(&chareink_17_italic);
EpdFontFamily chareink17FontFamily(&chareink17RegularFont, &chareink17BoldFont, &chareink17ItalicFont, nullptr);
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont, nullptr);
EpdFont bookerly13RegularFont(&bookerly_13_regular);
EpdFont bookerly13BoldFont(&bookerly_13_bold);
EpdFont bookerly13ItalicFont(&bookerly_13_italic);
EpdFontFamily bookerly13FontFamily(&bookerly13RegularFont, &bookerly13BoldFont, &bookerly13ItalicFont, nullptr);
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont, nullptr);
EpdFont bookerly15RegularFont(&bookerly_15_regular);
EpdFont bookerly15BoldFont(&bookerly_15_bold);
EpdFont bookerly15ItalicFont(&bookerly_15_italic);
EpdFontFamily bookerly15FontFamily(&bookerly15RegularFont, &bookerly15BoldFont, &bookerly15ItalicFont, nullptr);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont, nullptr);
EpdFont bookerly17RegularFont(&bookerly_17_regular);
EpdFont bookerly17BoldFont(&bookerly_17_bold);
EpdFont bookerly17ItalicFont(&bookerly_17_italic);
EpdFontFamily bookerly17FontFamily(&bookerly17RegularFont, &bookerly17BoldFont, &bookerly17ItalicFont, nullptr);
EpdFont vollkorn12RegularFont(&vollkorn_12_regular);
EpdFont vollkorn12BoldFont(&vollkorn_12_bold);
EpdFont vollkorn12ItalicFont(&vollkorn_12_italic);
EpdFontFamily vollkorn12FontFamily(&vollkorn12RegularFont, &vollkorn12BoldFont, &vollkorn12ItalicFont, nullptr);
EpdFont vollkorn13RegularFont(&vollkorn_13_regular);
EpdFont vollkorn13BoldFont(&vollkorn_13_bold);
EpdFont vollkorn13ItalicFont(&vollkorn_13_italic);
EpdFontFamily vollkorn13FontFamily(&vollkorn13RegularFont, &vollkorn13BoldFont, &vollkorn13ItalicFont, nullptr);
EpdFont vollkorn14RegularFont(&vollkorn_14_regular);
EpdFont vollkorn14BoldFont(&vollkorn_14_bold);
EpdFont vollkorn14ItalicFont(&vollkorn_14_italic);
EpdFontFamily vollkorn14FontFamily(&vollkorn14RegularFont, &vollkorn14BoldFont, &vollkorn14ItalicFont, nullptr);
EpdFont vollkorn15RegularFont(&vollkorn_15_regular);
EpdFont vollkorn15BoldFont(&vollkorn_15_bold);
EpdFont vollkorn15ItalicFont(&vollkorn_15_italic);
EpdFontFamily vollkorn15FontFamily(&vollkorn15RegularFont, &vollkorn15BoldFont, &vollkorn15ItalicFont, nullptr);
EpdFont vollkorn16RegularFont(&vollkorn_16_regular);
EpdFont vollkorn16BoldFont(&vollkorn_16_bold);
EpdFont vollkorn16ItalicFont(&vollkorn_16_italic);
EpdFontFamily vollkorn16FontFamily(&vollkorn16RegularFont, &vollkorn16BoldFont, &vollkorn16ItalicFont, nullptr);
EpdFont vollkorn17RegularFont(&vollkorn_17_regular);
EpdFont vollkorn17BoldFont(&vollkorn_17_bold);
EpdFont vollkorn17ItalicFont(&vollkorn_17_italic);
EpdFontFamily vollkorn17FontFamily(&vollkorn17RegularFont, &vollkorn17BoldFont, &vollkorn17ItalicFont, nullptr);

EpdFont unifont14RegularFont(&unifont_14_regular);
EpdFontFamily unifont14FontFamily(&unifont14RegularFont, nullptr, nullptr, nullptr, 1, 0, false);
EpdFont unifont18RegularFont(&unifont_18_regular);
EpdFontFamily unifont18FontFamily(&unifont18RegularFont, nullptr, nullptr, nullptr, 1, 0, false);

// Bitter: slab-serif reader font. Regular, Bold, Italic (no BoldItalic
// source TTF -- slot nullptr, Vollkorn pattern). Exposed at sizes 12, 14,
// 16 only (odd sizes and 17 dropped to save flash).
EpdFont bitter12RegularFont(&bitter_12_regular);
EpdFont bitter12BoldFont(&bitter_12_bold);
EpdFont bitter12ItalicFont(&bitter_12_italic);
EpdFontFamily bitter12FontFamily(&bitter12RegularFont, &bitter12BoldFont, &bitter12ItalicFont, nullptr);
EpdFont bitter14RegularFont(&bitter_14_regular);
EpdFont bitter14BoldFont(&bitter_14_bold);
EpdFont bitter14ItalicFont(&bitter_14_italic);
EpdFontFamily bitter14FontFamily(&bitter14RegularFont, &bitter14BoldFont, &bitter14ItalicFont, nullptr);
EpdFont bitter16RegularFont(&bitter_16_regular);
EpdFont bitter16BoldFont(&bitter_16_bold);
EpdFont bitter16ItalicFont(&bitter_16_italic);
EpdFontFamily bitter16FontFamily(&bitter16RegularFont, &bitter16BoldFont, &bitter16ItalicFont, nullptr);

// Galmuri: Korean pixel font. Regular-only headers -- italic synthesized
// via slant, bold via multi-pass redraw (1 base + 2 extra for visible
// weight). Sizes 11, 12, 14. Size 10 dropped (too small on screen).
EpdFont galmuri11RegularFont(&galmuri_11_regular);
EpdFontFamily galmuri11FontFamily(&galmuri11RegularFont, nullptr, nullptr, nullptr, 1, 2, true);
EpdFont galmuri12RegularFont(&galmuri_12_regular);
EpdFontFamily galmuri12FontFamily(&galmuri12RegularFont, nullptr, nullptr, nullptr, 1, 2, true);
EpdFont galmuri14RegularFont(&galmuri_14_regular);
EpdFontFamily galmuri14FontFamily(&galmuri14RegularFont, nullptr, nullptr, nullptr, 1, 2, true);

EpdFont smallFont(&ui_8_regular);
EpdFontFamily smallFontFamily(&smallFont, nullptr, nullptr, nullptr, 0, 0, false);
EpdFont ui10RegularFont(&ui_10_regular);
EpdFontFamily ui10FontFamily(&ui10RegularFont, nullptr, nullptr, nullptr, 0, 0, false);
EpdFont ui12RegularFont(&ui_12_regular);
EpdFontFamily ui12FontFamily(&ui12RegularFont, nullptr, nullptr, nullptr, 0, 0, false);

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

// Folder-dirty state lives in crosspoint::sleep::WallpaperPlaylist; it is
// marked dirty before entering activities that can modify /sleep (e.g. file
// transfer) and reconciled by trimSleepFolderIfDirty() on the Home route.

static crosspoint::sleep::SdFatSleepFs s_sleepFs;

static void wireWallpaperPlaylist() {
  crosspoint::sleep::WallpaperPlaylist::Deps deps;
  deps.fs = &s_sleepFs;
  deps.playlist = &APP_STATE.sleepImagePlaylist;
  deps.lastShownFilename = &APP_STATE.lastShownSleepFilename;
  deps.cursor = &APP_STATE.lastSleepImage;
  deps.lastRenderedPath = &APP_STATE.lastSleepWallpaperPath;
  // WallpaperPlaylist::advance() runs inside SleepActivity::onEnter, AFTER
  // enterDeepSleep's persistAppState flush and milliseconds before the CPU
  // enters deep sleep. saveToFile() is debounced — the debounce window
  // never fires, so the new lastShownSleepFilename is lost and the next
  // boot loads the stale value (same wallpaper every wake). Force a sync
  // flush so rotation survives the deep-sleep boundary.
  deps.saveState = []() {
    const bool ok = APP_STATE.saveToFile();
    crosspoint::persist::PersistManager().flushAll();
    return ok;
  };
  deps.randomFn = [](long mod) -> long { return ::random(mod); };
  deps.isFavorite = [](const std::string& path) { return FavoriteBmp::isFavoritePath(path); };
  deps.onPathRenamed = [](const std::string& from, const std::string& to) {
    FavoriteBmp::replacePathReferences(from, to);
  };
  deps.onBeforeTrimMove = []() {};  // no popup in current call sites
  crosspoint::sleep::WallpaperPlaylist::instance().setDeps(deps);

#if FEATURE_WALLPAPER_V2
  // V2: parallel wiring for unified shuffled rotation. Same APP_STATE pointers
  // (paused mode + lastShownSleepFilename are shared with V1 fallback). Buffer
  // + cursor live in /.crosspoint/sleep_order.txt via a dedicated stateless
  // SdFatFileIO instance.
  static crosspoint::persist::SdFatFileIO s_v2FileIO;
  crosspoint::sleep::v2::WallpaperPlaylistV2::Deps v2deps;
  v2deps.fs = &s_sleepFs;
  v2deps.fileIO = &s_v2FileIO;
  v2deps.lastShownFilename = &APP_STATE.lastShownSleepFilename;
  v2deps.lastRenderedPath = &APP_STATE.lastSleepWallpaperPath;
  v2deps.saveAppState = []() {
    const bool ok = APP_STATE.saveToFile();
    crosspoint::persist::PersistManager().flushAll();
    return ok;
  };
  v2deps.randomFn = [](long mod) -> long { return ::random(mod); };
  v2deps.isFavorite = [](const std::string& path) { return FavoriteBmp::isFavoritePath(path); };
  v2deps.onPathRenamed = [](const std::string& from, const std::string& to) {
    FavoriteBmp::replacePathReferences(from, to);
  };
  // PR2: wire to APP_STATE notification flags consumed by HomeActivity.
  v2deps.onTrimMoved = [](uint16_t /*moved*/) {};
  v2deps.onFavoritesCapBlocked = []() {};
  crosspoint::sleep::v2::WallpaperPlaylistV2::instance().setDeps(v2deps);
#endif
}

static void trimSleepFolderIfDirty() {
#if FEATURE_WALLPAPER_V2
  auto& wpv2 = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
  if (wpv2.dirty()) wpv2.reconcile();
#else
  auto& wp = crosspoint::sleep::WallpaperPlaylist::instance();
  if (wp.dirty()) wp.reconcile();
#endif
}

// Inline Reader activity construction. Used by both the V2 factory and the
// boot-time resume path in setup() (which bypasses the router since the main
// loop has not yet started draining pending transitions).
static void openReaderInline(const std::string& initialEpubPath) {
  const std::string bookPath = initialEpubPath;  // Copy before exitActivity() invalidates the reference
  // Explicit reset ensures the "Opening book..." toast starts a fresh
  // stack with a fresh sShownAtMs timestamp — otherwise leftover state
  // from a prior StatusPopup or unclosed toast would leave sActive==true,
  // causing show() to skip its first-in-stack timer reset and
  // maybeShowStillWorkingToast() to fire the "Long chapter..." popup
  // instantly against a stale timestamp.
  TransitionFeedback::resetStacking();
  TransitionFeedback::show(renderer, tr(STR_OPENING_BOOK));
  exitActivity();
  enterNewActivity(new ReaderActivity(renderer, mappedInputManager, bookPath, onGoHome, onGoToMyLibraryWithPath));
}

void onGoToReader(const std::string& initialEpubPath) {
  lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Reader, initialEpubPath});
}

static void openFileTransferInline() {
  TransitionFeedback::show(renderer, tr(STR_STARTING_SERVER));
#if FEATURE_WALLPAPER_V2
  crosspoint::sleep::v2::WallpaperPlaylistV2::instance().markFolderDirty();
#else
  crosspoint::sleep::WallpaperPlaylist::instance().markFolderDirty();
#endif
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToFileTransfer() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::FileTransfer, ""}); }

void onGoToSettings() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Settings, ""}); }

// ActivityRouter applies persist policy before calling this factory (RFC #23).
static void openMyLibraryInline(const std::string& path) {
  TransitionFeedback::show(renderer, tr(STR_LOADING_LIBRARY));
  exitActivity();
  if (path.empty()) {
    enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
  } else {
    enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader, path));
  }
}

void onGoToMyLibrary() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::MyLibrary, ""}); }

static void openRecentBooksInline() {
  TransitionFeedback::show(renderer, tr(STR_LOADING_RECENTS));
  exitActivity();
  enterNewActivity(new RecentBooksActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToRecentBooks() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::RecentBooks, ""}); }

void onGoToMyLibraryWithPath(const std::string& path) {
  lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::MyLibraryAt, path});
}

static void openBrowserInline() {
  TransitionFeedback::show(renderer, tr(STR_LOADING_BROWSER));
  exitActivity();
  enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToBrowser() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Browser, ""}); }

static void openHomeInline() {
  TransitionFeedback::show(renderer, tr(STR_LOADING_HOME));
  exitActivity();
  enterNewActivity(new HomeActivity(renderer, mappedInputManager, onGoToReader, onGoToMyLibrary, onGoToRecentBooks,
                                    onGoToSettings, onGoToFileTransfer, onGoToBrowser));
}

void onGoHome() { lifecycle::ActivityRouter::instance().request({lifecycle::RouteId::Home, ""}); }

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
extern "C" void onHeapAllocFailed(size_t requested, uint32_t caps, const char* function_name) {
  // No std::string / no LOG_DIAG here — Logging may itself allocate.
  ets_printf("[DIAG] [HEAP] alloc-fail size=%u caps=%lu fn=%s\n", static_cast<unsigned>(requested),
             static_cast<unsigned long>(caps), function_name ? function_name : "?");
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
  }
}

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  LOG_DBG("MAIN", "Display initialized");
  renderer.insertFont(CHAREINK_12_FONT_ID, chareink12FontFamily);
  renderer.insertFont(CHAREINK_13_FONT_ID, chareink13FontFamily);
  renderer.insertFont(CHAREINK_14_FONT_ID, chareink14FontFamily);
  renderer.insertFont(CHAREINK_15_FONT_ID, chareink15FontFamily);
  renderer.insertFont(CHAREINK_16_FONT_ID, chareink16FontFamily);
  renderer.insertFont(CHAREINK_17_FONT_ID, chareink17FontFamily);
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_13_FONT_ID, bookerly13FontFamily);
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
  renderer.insertFont(BOOKERLY_15_FONT_ID, bookerly15FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_17_FONT_ID, bookerly17FontFamily);
  renderer.insertFont(VOLLKORN_12_FONT_ID, vollkorn12FontFamily);
  renderer.insertFont(VOLLKORN_13_FONT_ID, vollkorn13FontFamily);
  renderer.insertFont(VOLLKORN_14_FONT_ID, vollkorn14FontFamily);
  renderer.insertFont(VOLLKORN_15_FONT_ID, vollkorn15FontFamily);
  renderer.insertFont(VOLLKORN_16_FONT_ID, vollkorn16FontFamily);
  renderer.insertFont(VOLLKORN_17_FONT_ID, vollkorn17FontFamily);
  renderer.insertFont(UNIFONT_14_FONT_ID, unifont14FontFamily);
  renderer.insertFont(UNIFONT_18_FONT_ID, unifont18FontFamily);
  renderer.insertFont(BITTER_12_FONT_ID, bitter12FontFamily);
  renderer.insertFont(BITTER_14_FONT_ID, bitter14FontFamily);
  renderer.insertFont(BITTER_16_FONT_ID, bitter16FontFamily);
  renderer.insertFont(GALMURI_11_FONT_ID, galmuri11FontFamily);
  renderer.insertFont(GALMURI_12_FONT_ID, galmuri12FontFamily);
  renderer.insertFont(GALMURI_14_FONT_ID, galmuri14FontFamily);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  static FontCacheManager fontCacheManager(renderer.getFontMap());
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  // Register the heap allocator failure hook now that FCM is reachable
  // via renderer. Idempotent: a re-call replaces the previous handler.
  // ESP-IDF's documented contract — runs on the failing task, retries
  // the allocation once after we return — is what makes this useful as
  // a defragmentation trigger rather than just a logging hook.
  if (heap_caps_register_failed_alloc_callback(&onHeapAllocFailed) != ESP_OK) {
    LOG_ERR("MAIN", "heap_caps_register_failed_alloc_callback failed");
  }
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

// Boot heap stage probe: dumps free/largest/free-blocks at named boot
// checkpoints so we can see which stage strands the heap into small
// fragments. PR #104 follow-up.
static void logHeapStage(const char* label) {
  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_8BIT);
  LOG_DIAG("BOOT", "stage='%s' free=%u largest=%u free_blocks=%u alloc_blocks=%u total=%u", label,
           (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block, (unsigned)info.free_blocks,
           (unsigned)info.allocated_blocks, (unsigned)(info.total_free_bytes + info.total_allocated_bytes));
}

void setup() {
  t1 = millis();

  gpio.begin();
  powerManager.begin();
  HalSystem::begin();

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
    enterNewActivity(
        new FullScreenMessageActivity(renderer, mappedInputManager, "SD card error", EpdFontFamily::REGULAR));
    return;
  }

  // Dump crash report to SD card if we rebooted from a panic
  HalSystem::checkPanic();
  HalSystem::clearPanic();

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

  // Lazy BLE init: only start stack if a device was previously paired
  if (SETTINGS.bleEnabled && SETTINGS.bleDeviceAddr[0] != '\0') {
    LOG_INF("BLE", "Saved BLE device found, initializing for auto-reconnect");
    if (BLE_HID.init()) {
      BLE_HID.tryAutoReconnect();
    }
  }

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

  exitActivity();
  auto* bootActivity = new BootActivity(renderer, mappedInputManager);
  enterNewActivity(bootActivity);

  bootActivity->setProgress(32, "Restoring state");
  {
    // Touch the store so it registers with PersistManager before the sidecar
    // backup runs (backup iterates registered store paths).
    (void)crosspoint::persist::appStateStore();
    crosspoint::persist::SdFatFileIO sidecarIo;
    if (crosspoint::persist::PersistManager().backupSidecarIfNewFirmware(sidecarIo, CROSSPOINT_VERSION)) {
      LOG_INF("MAIN", "First boot of %s — SD sidecar backup written", CROSSPOINT_VERSION);
    }
  }
  APP_STATE.loadFromFile();
  logHeapStage("after_app_state");

  // Wire crosspoint::sleep::WallpaperPlaylist deps now that APP_STATE is populated — all
  // subsequent sleep paths (trimSleepFolderIfDirty, SleepActivity) read through
  // the module.
  wireWallpaperPlaylist();

  LOG_INF("MAIN", "Booting complete, checking initial activity");

  // Always load recents early — reader activities call addBook() which saves
  // to disk, so an unloaded list would overwrite the file with just one entry.
  RECENT_BOOKS.loadFromFile();
  logHeapStage("after_recents");

  // Safety: skip straight to reader if Back held or crash-loop detected.
  const bool forcedHome =
      mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0;

  // Build list of .epub books from recents for boot-into-book logic.
  std::vector<const RecentBook*> recentEpubs;
  if (!forcedHome) {
    for (const auto& b : RECENT_BOOKS.getBooks()) {
      if (b.path.size() > 5 && b.path.rfind(".epub") == b.path.size() - 5) {
        recentEpubs.push_back(&b);
      }
    }
  }

  // Determine boot destination.
  // Always open a book when possible: random pick or most-recent epub.
  // Fall back to home only if forced or no epubs in recents.
  std::string readerPath;
  if (!forcedHome && !recentEpubs.empty()) {
    if (SETTINGS.randomBookOnBoot && recentEpubs.size() > 1) {
      readerPath = recentEpubs[random(static_cast<long>(recentEpubs.size()))]->path;
    } else {
      readerPath = recentEpubs.front()->path;
    }
  }

  const bool goHome = readerPath.empty();

  if (!goHome) {
    APP_STATE.openEpubPath = "";
    if (APP_STATE.readerActivityLoadCount < 255) APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
  }

  // Defer sleep cache trimming until home screen is actually needed.
  if (goHome) {
    bootActivity->setProgress(60, "Refreshing sleep cache");
#if FEATURE_WALLPAPER_V2
    auto& wp = crosspoint::sleep::v2::WallpaperPlaylistV2::instance();
#else
    auto& wp = crosspoint::sleep::WallpaperPlaylist::instance();
#endif
    wp.markFolderDirty();
    wp.reconcile();
  }
  bootActivity->setProgress(80, goHome ? "Preparing home" : "Resuming book");

  // Wire ActivityRouter with Deps + route factories (RFC #23). All transitions
  // flow through the router: per-route persist/trim policy is applied before
  // the factory runs, and deep-sleep entry follows the fixed hook sequence in
  // ActivityRouter::enterDeepSleep.
  {
    auto& router = lifecycle::ActivityRouter::instance();

    lifecycle::ActivityRouter::Deps deps;
    deps.currentActivitySlot = &currentActivity;
    deps.persistAppState = &::persistAppState;
    deps.trimSleepFolderIfDirty = &::trimSleepFolderIfDirty;
    deps.onBeforeDeepSleep = [](bool fromReader) {
      if (BLE_HID.isInitialized()) {
        BLE_HID.disconnect();
        BLE_HID.deinit();
      }
      APP_STATE.lastSleepFromReader = fromReader;
    };
    deps.onAfterDeepSleep = []() {
      display.deepSleep();
      LOG_DBG("MAIN", "Power button press calibration value: %lu ms", t2 - t1);
      LOG_DBG("MAIN", "Entering deep sleep");
      powerManager.startDeepSleep(gpio);
    };
    deps.enterSleepActivity = []() { enterNewActivity(new SleepActivity(renderer, mappedInputManager)); };
    router.setDeps(std::move(deps));

    router.setRouteFactory(lifecycle::RouteId::Settings, [](const std::string& /*payload*/) {
      TransitionFeedback::show(renderer, tr(STR_LOADING_SETTINGS));
      exitActivity();
      enterNewActivity(new SettingsActivity(renderer, mappedInputManager, onGoHome));
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

  bootActivity->setProgress(100, goHome ? "Opening home" : "Opening book");

  // Phase 1 BDF custom-font scan. Runs after APP_STATE is loaded so the
  // seen/skipped vectors are populated. If any new BDF is queued, the popup
  // replaces BootActivity; the final dismiss callback transitions to the
  // boot destination via ActivityRouter::begin (synchronous dispatch).
  auto launchBootDestination = [goHome, readerPath]() {
    if (goHome) {
      lifecycle::ActivityRouter::instance().begin({lifecycle::RouteId::Home, ""});
    } else {
      lifecycle::ActivityRouter::instance().begin({lifecycle::RouteId::Reader, readerPath});
    }
  };

  // Ensure /custom-font/ exists so the web UI can drop freshly-baked
  // .bin files there without first creating the directory by hand.
  // mkdir is idempotent; harmless when the dir already exists.
  Storage.mkdir("/custom-font");

  // One-shot: on the first boot after updating to the .bin pipeline, wipe
  // any leftover .bdf / .idx files from the previous BDF pipeline. A flag
  // in state.json keeps this from re-running every boot.
  crosspoint::fonts::cleanupLegacyBdfFiles();

  auto& customFonts = crosspoint::fonts::CustomBinFontManager::instance();
  customFonts.setRenderer(&renderer);
  customFonts.scan();

  // Safety net: if SETTINGS picks a custom family whose named .bin is
  // not actually installed (e.g. deleted via the web UI, SD pulled,
  // etc.), or the regular variant exists but its CPBN header is
  // malformed (corrupted upload, FW format change), fall back to the
  // default built-in family so the reader doesn't silently render empty
  // text. The header validation runs once at boot via the static
  // validateFile helper — no file is held open afterward.
  LOG_DIAG("CFONT", "boot fallback check: ff=%u customFont='%s' cfsPt=%u", (unsigned)SETTINGS.fontFamily,
           SETTINGS.customFontName.c_str(), (unsigned)SETTINGS.customFontSizePt);
  if (SETTINGS.fontFamily == CrossPointSettings::CUSTOM_FAMILY) {
    bool ok = false;
    const char* failReason = "missing";
    if (!SETTINGS.customFontName.empty()) {
      bool sizeInstalled = false;
      const auto sizes = customFonts.installedSizesFor(SETTINGS.customFontName);
      for (uint8_t s : sizes) {
        if (s == SETTINGS.customFontSizePt) {
          sizeInstalled = true;
          break;
        }
      }
      if (sizeInstalled) {
        if (crosspoint::fonts::CustomBinFontManager::validateInstalledRegular(SETTINGS.customFontName,
                                                                              SETTINGS.customFontSizePt)) {
          ok = true;
        } else {
          failReason = "invalid CPBN header";
        }
      }
    }
    if (!ok) {
      LOG_DIAG("CFONT", "boot revert: active custom font '%s' size=%u %s; falling back to CHAREINK 12 crisp",
               SETTINGS.customFontName.c_str(), static_cast<unsigned>(SETTINGS.customFontSizePt), failReason);
      SETTINGS.fontFamily = CrossPointSettings::CHAREINK;
      SETTINGS.fontSize = CrossPointSettings::SIZE_12;
      SETTINGS.textRenderMode = CrossPointSettings::TEXT_RENDER_CRISP;
      SETTINGS.customFontName.clear();
      SETTINGS.customFontSizePt = 0;
      SETTINGS.saveToFile();
    }
  }
  LOG_DIAG("CFONT", "boot fallback done: ff=%u customFont='%s' cfsPt=%u", (unsigned)SETTINGS.fontFamily,
           SETTINGS.customFontName.c_str(), (unsigned)SETTINGS.customFontSizePt);
  launchBootDestination();

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

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
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes", ESP.getFreeHeap(), ESP.getHeapSize(),
            ESP.getMinFreeHeap());
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

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || (currentActivity && currentActivity->preventAutoSleep())) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  auto triggerDeepSleep = []() {
    // Shown on both auto-sleep (inactivity timeout) and power-button-hold paths
    // so the device never blanks silently. renderer is a file-scope global.
    TransitionFeedback::show(renderer, tr(STR_GOING_TO_SLEEP));
    const bool fromReader = currentActivity && currentActivity->isReaderActivity();
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
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
