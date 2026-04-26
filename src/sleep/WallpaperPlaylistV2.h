/**
 * @file WallpaperPlaylistV2.h
 * @brief Unified shuffled wallpaper rotation (FEATURE_WALLPAPER_V2).
 *
 * Single contiguous-buffer shuffle queue for /sleep wallpapers. Replaces the
 * Small/Large bifurcation in WallpaperPlaylist.cpp with one code path that
 * scales from 4 to 500 files without per-entry std::string heap allocation.
 *
 * Storage shape:
 *   buffer_ = "name1\nname2\nname3\n..."  // single std::string, one heap block
 *   cursor_ = byte offset of next-to-show name
 *
 * Persistence: separate text file at /.crosspoint/sleep_order.txt via the
 * persist::IFileIO atomic-write path (.tmp → .bak → real). state.json schema
 * unchanged on the playlist field — buffer never goes through ArduinoJson, so
 * the heap-fragmentation regression that produced PR #104 cannot recur here.
 *
 * Semantics:
 *   - Default order: shuffled (Fisher-Yates on first build / on reshuffle).
 *   - Auto-reshuffle when cursor reaches end of buffer (every "lap").
 *   - New files added to /sleep get inserted at cursor in mtime order so
 *     they appear in the next unlocks.
 *   - Strict cap at 500: on overflow, oldest-mtime non-favorites are pushed to
 *     /sleep pause. If favorites alone fill the cap, new uploads land in
 *     /sleep pause and a notification flag fires.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../persist/IFileIO.h"
#include "SleepFs.h"

namespace crosspoint {
namespace sleep {
namespace v2 {

// Hard cap on /sleep contents. Mirrors CrossPointState::SLEEP_FAVORITES_MAX.
// One contiguous allocation of ~10 KB at this cap on the C3 — well under the
// observed 26 KB min-largest-free-block low-water mark.
constexpr size_t kSleepFolderCap = 500;

class WallpaperPlaylistV2 {
 public:
  struct Deps {
    ISleepFs* fs = nullptr;
    persist::IFileIO* fileIO = nullptr;
    std::string orderFilePath = "/.crosspoint/sleep_order.txt";

    std::string* lastShownFilename = nullptr;
    std::string* lastRenderedPath = nullptr;

    std::function<bool()> saveAppState;
    std::function<long(long)> randomFn;
    std::function<bool(const std::string&)> isFavorite;
    std::function<void(const std::string& /*from*/, const std::string& /*to*/)> onPathRenamed;
    std::function<void(uint16_t /*movedCount*/)> onTrimMoved;
    std::function<void()> onFavoritesCapBlocked;
  };

  static WallpaperPlaylistV2& instance();

  void setDeps(const Deps&);
  const Deps& deps() const { return deps_; }
  void resetForTest();

  void markFolderDirty() { dirty_ = true; }
  bool dirty() const { return dirty_; }

  void reconcile();
  std::string advance();
  bool reshuffle();
  void rememberRendered(const std::string& fullPath, const std::string& filename = "");
  void clearRenderedPath();

  const std::string& bufferForTest() const { return buffer_; }
  size_t cursorForTest() const { return cursor_; }
  size_t entryCountForTest() const;

 private:
  WallpaperPlaylistV2() = default;

  bool ensureLoaded();
  bool loadFromDisk();
  bool saveToDisk() const;
  void writeBuffer(const std::vector<std::string>& names, size_t cursor);
  std::vector<std::string> bufferEntries() const;
  std::string peekAtCursor() const;
  void advanceCursor();
  uint16_t trimToCap(std::vector<SleepBmpEntry>& entries, bool& favoritesCapBlocked);

  // Heap-cheap membership check via direct substring scan of buffer_. Avoids
  // building the (~500-element) hash_set that materializing bufferEntries()
  // would require. O(name + buffer_size) per query.
  bool nameIsInBuffer(const std::string& name) const;

  Deps deps_;
  std::string buffer_;
  size_t cursor_ = 0;
  bool dirty_ = true;
  bool loaded_ = false;
};

}  // namespace v2
}  // namespace sleep
}  // namespace crosspoint
