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
 *   - Default order: sequential by mtime descending (newest first / on top).
 *     Initial build and post-trim rebuild use rebuildSequential() — no shuffle.
 *   - On lap end, cursor resets to 0 and replays the same order. The buffer
 *     is NEVER auto-shuffled; only the user-initiated reshuffle() (settings
 *     "shuffle now") randomizes.
 *   - New files added to /sleep get spliced at the FRONT of the buffer and
 *     cursor resets to 0, so freshly uploaded wallpapers show next. A
 *     favorite/unfavorite RENAME (same image, toggled _F suffix) is NOT a new
 *     file: reconcile() replaces the old name in place so the image keeps its
 *     rotation slot instead of jumping to the front and re-showing.
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
    // Returns the largest contiguous free heap block in bytes. Production
    // wires heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT). Host
    // tests inject a stub to simulate fragmentation. If unset, the playlist
    // assumes unlimited heap (treats every reserve as safe) — matches the
    // pre-RFC #156 host behaviour where the heap probe was ifdef'd out.
    std::function<size_t()> largestFreeBlockFn;
    // Maps a /sleep basename to its favorite-toggle counterpart: "x.bmp" ->
    // "x_F.bmp" and "x_F.bmp" -> "x.bmp". Lets reconcile() recognize a
    // favorite/unfavorite RENAME (same image, toggled _F suffix) as distinct
    // from a fresh upload, so it replaces the name in place and keeps the
    // image's rotation slot instead of splicing it to the front. Returns the
    // input unchanged for non-images. If unset, reconcile skips rename
    // detection and treats a renamed file as new (pre-fix behaviour).
    std::function<std::string(const std::string&)> favoriteCounterpartFn;
  };

  // Outcome of the most recent reconcile, surfaced as data (RFC #145).
  // Replaces the former onTrimMoved / onFavoritesCapBlocked callback slots,
  // which were wired to noops in production and so could never reach the UI.
  // The facade drains this after advance() and maps it onto persistent
  // APP_STATE flags for the next-wake home warning / toast.
  struct Notice {
    uint16_t movedToPause = 0;         // non-favorites demoted to /sleep pause this reconcile
    bool favoritesCapBlocked = false;  // favorites alone saturate the 500 cap, new uploads blocked
    bool reconciled = false;           // a reconcile actually ran, so favoritesCapBlocked is fresh
    bool any() const { return movedToPause > 0 || favoritesCapBlocked; }
  };

  static WallpaperPlaylistV2& instance();

  void setDeps(const Deps&);
  const Deps& deps() const { return deps_; }
  void resetForTest();

  void markFolderDirty() { dirty_ = true; }
  bool dirty() const { return dirty_; }

  void reconcile();
  std::string advance();
  // User-initiated shuffle (settings "shuffle now"). Fisher-Yates randomize +
  // cursor reset. Internal lap-end / empty-buffer rebuilds use the sequential
  // rebuildSequential() path instead.
  bool reshuffle();
  void rememberRendered(const std::string& fullPath, const std::string& filename = "");
  void clearRenderedPath();

  // Read and clear the pending reconcile notice (RFC #145). Returns a zeroed
  // Notice when no reconcile has run since the last drain.
  Notice takeNotice();

  const std::string& bufferForTest() const { return buffer_; }
  size_t cursorForTest() const { return cursor_; }
  size_t entryCountForTest() const;

 private:
  WallpaperPlaylistV2() = default;

  bool ensureLoaded();
  bool loadFromDisk();
  // Rebuild buffer from /sleep in sequential newest-first order. Default
  // rebuild path used by reconcile / advance when buffer_ is empty. Returns
  // true iff the rebuilt buffer is non-empty.
  bool rebuildSequential();
  bool saveToDisk() const;
  void writeBuffer(const std::vector<std::string>& names, size_t cursor);
  std::string peekAtCursor() const;
  void advanceCursor();
  uint16_t trimToCap(std::vector<SleepBmpEntry>& entries, bool& favoritesCapBlocked);

  // Heap-cheap membership check: walk the '\n'-delimited names in buffer_ and
  // compare each line to `name` in place — zero allocation, no temporary needle
  // or materialized name set. O(buffer_size) per query. The char* overload lets
  // the reconcile walk query straight off the SD layer's stack buffer; the
  // std::string overload forwards to it.
  bool nameIsInBuffer(const char* name, size_t len) const;
  bool nameIsInBuffer(const std::string& name) const;

  // Replace a whole-line entry `oldName` with `newName` in place, preserving
  // its rotation slot and adjusting cursor_ for the length change if the entry
  // sits before the cursor. Used by reconcile() to fold a favorite/unfavorite
  // rename into the existing order rather than re-adding it at the front.
  // Returns false (no change) if `oldName` is absent or the heap can't afford
  // the (tiny) growth — the caller then treats the file as new, as before.
  bool renameInBuffer(const std::string& oldName, const std::string& newName);

  // Probe the heap for a contiguous block large enough for `needBytes`
  // plus the standard 4 KB transient-growth headroom. Consults
  // deps_.largestFreeBlockFn; if unset, treats the heap as unlimited.
  // Build is compiled -fno-exceptions so the playlist must probe before
  // any std::string::reserve that might bad_alloc.
  bool heapHasContiguous(size_t needBytes) const;

  Deps deps_;
  std::string buffer_;
  size_t cursor_ = 0;
  bool dirty_ = true;
  bool loaded_ = false;
  Notice pendingNotice_;
};

}  // namespace v2
}  // namespace sleep
}  // namespace crosspoint
