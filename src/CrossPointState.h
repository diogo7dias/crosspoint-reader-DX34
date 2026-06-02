/**
 * @file CrossPointState.h
 * @brief Runtime state persisted across reboots (current book, sleep playlist, etc.).
 *
 * Unlike CrossPointSettings (user preferences), CrossPointState tracks
 * session data: which book is open, current page, sleep image playlist,
 * recent books list, and reading statistics. Saved to /.crosspoint/state.json.
 *
 * Sleep playlist management has two paths:
 *   - Small collections (<=SLEEP_PLAYLIST_MAX_PERSIST): full shuffle in RAM.
 *   - Large collections (>200): sequential advance with lastShownSleepFilename.
 */
#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

class CrossPointState {
 public:
  // Collections larger than this threshold are handled without a full in-memory
  // playlist; only the last-shown filename is persisted in that case.
  // Why 30: each persisted entry is one std::string heap allocation
  // (~30-50 bytes) plus an ArduinoJson JsonVariant during deserialize.
  // At 500 the boot-time deserialize of state.json scattered ~270 small
  // blocks across the heap, splitting the largest contiguous block from
  // ~63 KB to ~17 KB — small enough that every book open hit the
  // pre-flight gate. Capping at 30 keeps shuffle UX for small wallpaper
  // collections; bigger collections fall through to the sequential
  // lastShownSleepFilename path which never holds the playlist in RAM.
  static constexpr size_t SLEEP_PLAYLIST_MAX_PERSIST = 30;

  // User-facing cap on protected ([F]-suffixed) sleep wallpapers.
  // Counted from disk in countProtectedSleepFavorites(), so this does
  // not pressure the heap the way the in-memory playlist does. Kept at
  // the historical 500 so users who already curated large favorite
  // collections do not see "Sleep favorites full" after the playlist
  // cap was lowered.
  static constexpr size_t SLEEP_FAVORITES_MAX = 500;

  std::string openEpubPath;
  uint8_t lastSleepImage = UINT8_MAX;  // UINT8_MAX = unset sentinel
  std::vector<std::string> sleepImagePlaylist;
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
