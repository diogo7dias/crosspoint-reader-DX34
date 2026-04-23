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
  static constexpr size_t SLEEP_PLAYLIST_MAX_PERSIST = 500;

  std::string openEpubPath;
  uint8_t lastSleepImage = UINT8_MAX;  // UINT8_MAX = unset sentinel
  std::vector<std::string> sleepImagePlaylist;
  std::string lastShownSleepFilename;
  std::string lastSleepWallpaperPath;
  std::vector<std::string> favoriteBmpPaths;
  // Phase 1 BDF custom-font tracking. Filenames (basename only, e.g.
  // "unifont_16.bdf") of custom fonts the user has acknowledged via the
  // boot prompt. seen = "Install" tapped (Phase 1 = log-only). skipped =
  // "Skip forever" tapped (never prompt again).
  std::vector<std::string> seenCustomFonts;
  std::vector<std::string> skippedCustomFonts;
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

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
