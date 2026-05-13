/**
 * @file Wallpaper.h
 * @brief Free-function facade over the sleep wallpaper rotation (RFC #145).
 *
 * Hides the V2 impl class, the production fs/IFileIO adapters, the APP_STATE
 * pointer wiring, and the deep-sleep flush sequencing. Production callers use
 * the namespaced free functions; tests opt in via Configure().
 *
 * Lazy init: on first call from any entry point, production deps are wired
 * inside this module. main.cpp does not need to call any setup function.
 *
 * Migration window: V1 (WallpaperPlaylist) and V2 (WallpaperPlaylistV2) both
 * remain in tree; this facade currently routes only to V2 (which is the
 * default for every build env in platformio.ini). V1 deletion is a follow-up.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "../persist/IFileIO.h"
#include "SleepFs.h"

namespace crosspoint {
namespace sleep {
namespace wallpaper {

// Re-export of the V2 hard cap, so callers do not reach into v2::.
constexpr size_t kSleepFolderCap = 500;

// Hot path: pick the next wallpaper basename (e.g. "abc.bmp"). Empty if
// /sleep is empty. Caller prepends "/sleep/" itself. Synchronously flushes
// APP_STATE so rotation survives the deep-sleep boundary.
std::string advance();

// Outcome of nextSleepFile() — everything the caller needs to render the
// picked wallpaper without consulting APP_STATE or recomputing the
// favorite display name.
struct SleepPick {
  std::string fullPath;     // "/sleep/foo.bmp" or empty if no image available
  std::string basename;     // basename portion, or empty for paused branch
  std::string displayName;  // FavoriteImage::displayNameForPath(fullPath)
  bool isPaused = false;    // facade routed via paused-rotation branch

  bool hasImage() const { return !fullPath.empty(); }
};

// Caller-supplied render attempt. Returns true if `pick.fullPath` was
// rendered successfully. Returning false signals a transient open/parse
// failure — the facade will pick another candidate and call probe again
// up to an internal retry budget.
using RenderProbe = std::function<bool(const SleepPick&)>;

// Hot-path entry point that hides heap-fragmentation strategy, the
// paused-rotation branch, the playlist-vs-direct-pick decision, the
// parse/open retry loop, and the post-render lastShown bookkeeping
// (RFC #156). Picks the next wallpaper given current heap state and
// on-disk content, drives `probe` until one renders successfully or the
// retry budget is exhausted.
//
// Returns the SleepPick that ultimately rendered (fullPath non-empty)
// or an empty pick if /sleep is empty or every retry failed.
//
// During the migration window this coexists with `advance()`; both
// share the same playlist state. New code should prefer this entry
// point.
SleepPick nextSleepFile(const RenderProbe& probe);

// Cold path: mark /sleep as needing a reconcile. Cheap. Reconcile work
// runs lazily inside the next advance() under the rich-sleep heap-budget
// gate. New files dropped via USB transfer become visible on the next
// sleep cycle, not the home screen immediately after disconnect.
void markFolderDirty();

// Settings-screen "shuffle now" — force a reshuffle. Returns false when
// /sleep is empty, true when at least one file was selected.
bool reshuffle();

// Post-render: remember the actually-rendered path (paused-mode dedup).
// `filename` is the basename portion; pass empty to leave lastShown alone.
void rememberRendered(const std::string& fullPath, const std::string& filename = "");

// Direct fs handle for callers that need to enumerate /sleep themselves
// (e.g. fragment-safe direct pick). Never null after first lazy init.
ISleepFs* fs();

// No-op in the V2 default path — reconcile is heap-gated and runs from
// inside advance(). Kept so the boot-route hook + ActivityRouter signature
// stay valid without conditional plumbing at the call site.
void reconcileIfDirty();

// Test/power-user opt-in. Production callers do not invoke this; the lazy
// init wires production adapters automatically. Tests call resetForTest()
// then Configure() with fakes.
struct Config {
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
  // Optional host-test seam: returns the largest contiguous free heap block
  // in bytes. Production lazy init wires heap_caps_get_largest_free_block
  // (MALLOC_CAP_DEFAULT) automatically; tests inject a stub to simulate
  // fragmentation cliffs. If left unset, the playlist treats the heap as
  // unlimited — matches the pre-RFC #156 host behaviour.
  std::function<size_t()> largestFreeBlockFn;
};

void Configure(const Config&);
void resetForTest();

}  // namespace wallpaper
}  // namespace sleep
}  // namespace crosspoint
