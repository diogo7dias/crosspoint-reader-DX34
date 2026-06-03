/**
 * @file CrossPointState.h
 * @brief Runtime state persisted across reboots (current book, sleep playlist, etc.).
 *
 * Unlike CrossPointSettings (user preferences), CrossPointState tracks
 * session data: which book is open, current page, recent books list, and
 * reading statistics. Saved to /.crosspoint/state.json.
 *
 * Sleep wallpaper rotation state lives in a separate order file
 * (/.crosspoint/sleep_order.txt) owned by the V2 playlist; only the
 * last-shown filename/path and the RFC #145 notice flags persist here.
 */
#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

class CrossPointState {
 public:
  // User-facing cap on protected ([F]-suffixed) sleep wallpapers.
  // Counted from disk in countProtectedSleepFavorites(), so this does
  // not pressure the heap the way the in-memory playlist does. Kept at
  // the historical 500 so users who already curated large favorite
  // collections do not see "Sleep favorites full" after the playlist
  // cap was lowered.
  static constexpr size_t SLEEP_FAVORITES_MAX = 500;

  std::string openEpubPath;
  std::string lastShownSleepFilename;
  std::string lastSleepWallpaperPath;
  std::vector<std::string> favoriteBmpPaths;
  // One-shot flag: set to 1 the first time the firmware successfully
  // walks /custom-font/ and deletes every leftover .bdf/.idx from the
  // previous BDF pipeline. Once flipped, the cleanup scan is skipped on
  // every subsequent boot. Stored as a small int rather than a bool so
  // future migrations can bump the value to re-trigger the cleanup.
  uint8_t customFontLegacyCleanupDone = 0;
  uint8_t readerActivityLoadCount = 0;
  uint32_t sessionPagesRead = 0;
  bool lastSleepFromReader = false;
  bool wallpaperRotationPaused = false;
  bool lastSleepWasQuotes = false;
  // RFC #145 wallpaper-rotation notices, set during sleep reconcile and
  // surfaced on the next home entry. sleepFavoritesCapReached is sticky state
  // (favorites alone fill the 500 cap, blocking new uploads); it drives the
  // home "favorites full" warning and clears once favorites drop below the cap.
  // pendingSleepWallpapersMovedToPause is a transient event count consumed once
  // by a home toast ("N wallpapers moved to /sleep pause/").
  bool sleepFavoritesCapReached = false;
  uint16_t pendingSleepWallpapersMovedToPause = 0;
  ~CrossPointState() = default;

  // Singleton — returns the data owned by persist::AppStateStore
  // (RFC #20). All 24 APP_STATE.x mutations + saveToFile() call sites
  // go through this accessor transparently, gaining debounce coalescing.
  static CrossPointState& getInstance();

  bool saveToFile() const;

  // Synchronous variant of saveToFile(): writes state.json on the calling
  // thread immediately instead of marking dirty for a debounced flush. Use at
  // crash-safety boundaries where the value MUST survive an imminent restart or
  // power-cycle (e.g. the boot crash-loop guard, which is set microseconds
  // before a book-open that may OOM-restart before the debounce ever drains).
  bool saveToFileSync() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
