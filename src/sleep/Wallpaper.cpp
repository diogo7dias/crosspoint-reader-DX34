#include "Wallpaper.h"

#include <Arduino.h>
#include <HalStorage.h>

#ifndef UNIT_TEST_HOST
#include <esp_heap_caps.h>
#endif

#include "../CrossPointState.h"
#include "../persist/PersistManager.h"
#include "../persist/SdFatFileIO.h"
#include "../util/FavoriteImage.h"
#include "HeapGuard.h"
#include "MemoryPolicy.h"
#include "SdFatSleepFs.h"
#include "WallpaperPlaylistV2.h"

namespace crosspoint {
namespace sleep {
namespace wallpaper {

namespace {

// Function-local statics: zero-init in BSS, constructed on first touch
// after Arduino framework init has run. Avoids static-init-order hazards
// with file-scope globals.
SdFatSleepFs& defaultFs() {
  static SdFatSleepFs s;
  return s;
}

persist::SdFatFileIO& defaultFileIO() {
  static persist::SdFatFileIO s;
  return s;
}

bool s_configured = false;

// Wire production deps onto the V2 impl. Idempotent: the configured flag
// short-circuits subsequent calls. Tests that want to override deps call
// resetForTest() first to drop the flag.
void ensureConfigured() {
  if (s_configured) return;
  s_configured = true;

  v2::WallpaperPlaylistV2::Deps d;
  d.fs = &defaultFs();
  d.fileIO = &defaultFileIO();
  d.lastShownFilename = &APP_STATE.lastShownSleepFilename;
  d.lastRenderedPath = &APP_STATE.lastSleepWallpaperPath;
  // advance() runs inside SleepActivity::onEnter, milliseconds before the
  // CPU enters deep sleep. The PersistManager debounce window never fires
  // before sleep, so we force a synchronous flush so rotation state
  // survives the deep-sleep boundary.
  d.saveAppState = []() {
    const bool ok = APP_STATE.saveToFile();
    crosspoint::persist::PersistManager().flushAll();
    return ok;
  };
  d.randomFn = [](long mod) -> long { return ::random(mod); };
  d.isFavorite = [](const std::string& path) { return FavoriteImage::isFavoritePath(path); };
  d.onPathRenamed = [](const std::string& from, const std::string& to) {
    FavoriteImage::replacePathReferences(from, to);
  };
  // Heap probe (RFC #156 C2): inject the device's contiguous-block query
  // through the playlist so the same code path runs under host tests with
  // a scripted heap. UNIT_TEST_HOST builds don't link esp_heap_caps; the
  // playlist treats nullptr as "unlimited heap".
#ifndef UNIT_TEST_HOST
  d.largestFreeBlockFn = []() { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); };
#endif

  v2::WallpaperPlaylistV2::instance().setDeps(d);
}

// Drain the reconcile notice (RFC #145) and fold it into persistent
// APP_STATE so the next-wake home screen can warn / toast. The favorites-cap
// flag is sticky state (refreshed only when a reconcile actually ran, so it
// clears once favorites drop below the cap); the moved-to-pause count is a
// transient event the home screen consumes once. Persists only when something
// changed — overflow is a rare event, so the extra synchronous flush before
// deep sleep stays off the common path.
void applyReconcileNotice() {
  const auto n = v2::WallpaperPlaylistV2::instance().takeNotice();
  bool changed = false;
  if (n.reconciled && APP_STATE.sleepFavoritesCapReached != n.favoritesCapBlocked) {
    APP_STATE.sleepFavoritesCapReached = n.favoritesCapBlocked;
    changed = true;
  }
  if (n.movedToPause > 0 && APP_STATE.pendingSleepWallpapersMovedToPause < 9999) {
    APP_STATE.pendingSleepWallpapersMovedToPause += n.movedToPause;
    changed = true;
  }
  if (changed) {
    APP_STATE.saveToFile();
    crosspoint::persist::PersistManager().flushAll();
  }
}

}  // namespace

std::string advance() {
  ensureConfigured();
  const std::string pick = v2::WallpaperPlaylistV2::instance().advance();
  applyReconcileNotice();
  return pick;
}

namespace {

// Retry budget for nextSleepFile's probe loop. Mirrors the
// kMaxParseRetries constant currently in SleepActivity. Until C4 of
// RFC #156 lands, both copies coexist.
constexpr int kNextSleepFileRetries = 5;

// Slack added on top of the measured sequential-playlist cost before deciding
// it's affordable. Covers small ancillary allocations during load/reconcile.
constexpr size_t kSeqGateHeadroom = 12 * 1024;

// Decide whether the sequential playlist (advance/reconcile/rebuild) can be
// materialized safely this cycle, or whether to fall back to the O(1)-heap
// direct pick.
//
// The previous fixed 64 KB gate (Op::ScanSleepPlaylist) ignored filename
// LENGTH. The order buffer is one byte per filename char, and on a rebuild the
// reconcile also builds a vector<SleepBmpEntry> plus per-name std::strings, and
// safeRead loads the order file via an Arduino String + a std::string copy
// (~2-3x the buffer, transiently). A folder of many long-named .pxc files
// therefore materializes to FAR more than 64 KB, so the gate opened and
// reconcile OOM-aborted on lock (the "can't lock" crash). This measures the
// REAL cost with a streaming, zero-heap walk (sums filename bytes) and requires
// both enough contiguous heap (largest single block) and enough total free.
// Conservative on purpose: under-gating only costs a direct pick; over-gating
// costs a brick.
bool sequentialPlaylistAffordable() {
  auto* sfs = v2::WallpaperPlaylistV2::instance().deps().fs;
  if (!sfs) return false;
  size_t count = 0;
  size_t bufferBytes = 0;  // == order-buffer size: sum of (filename length + 1)
  sfs->walkSleepBmps([&](const char* /*name*/, size_t len, uint32_t /*mtime*/) {
    ++count;
    bufferBytes += len + 1;
  });
  if (count == 0) return false;  // empty folder — direct pick returns empty too

  const size_t entryVecBytes = count * sizeof(crosspoint::sleep::SleepBmpEntry);
  // Largest single contiguous allocation the path makes: the order buffer, or
  // (on rebuild) the entry vector — whichever is bigger.
  const size_t contigNeed = (bufferBytes > entryVecBytes ? bufferBytes : entryVecBytes) + kSeqGateHeadroom;
  // Worst-case coexisting transient peak: order buffer + safeRead's String +
  // std::string copy (~3x buffer) plus the entry vector and its per-name strings.
  const size_t totalNeed = bufferBytes * 3 + entryVecBytes * 2 + kSeqGateHeadroom;

  return crosspoint::heap::largestFreeBlockBytes() >= contigNeed &&
         crosspoint::mem::totalFreeBytes() >= totalNeed;
}

// Streaming fragment-safe pick — direct mirror of SleepActivity's
// pickSleepFileDirect helper. Touches O(1) heap beyond the returned
// basename. Empty if /sleep is empty or fs not wired.
std::string pickDirectBasename() {
  auto* sfs = v2::WallpaperPlaylistV2::instance().deps().fs;
  if (!sfs) return {};
  const size_t count = sfs->countSleepBmps(kSleepFolderCap);
  if (count == 0) return {};
  // Anti-repeat: this random last-resort has no playlist cursor, so a naive
  // single roll can return the wallpaper we just showed (a 1/N chance —
  // glaring on small libraries). Re-roll a few times to avoid the last-shown
  // basename. With >1 file this converges almost immediately; with exactly
  // one file we accept it (nothing else to show). lastShownSleepFilename and
  // nthSleepBmp both deal in basenames, so the comparison is apples-to-apples.
  const std::string& lastShown = APP_STATE.lastShownSleepFilename;
  std::string pick;
  for (int tries = 0; tries < 5; ++tries) {
    const size_t idx = static_cast<size_t>(::random(static_cast<long>(count)));
    pick = sfs->nthSleepBmp(idx);
    if (count <= 1 || pick.empty() || pick != lastShown) break;
  }
  return pick;
}

SleepPick makePickFromBasename(const std::string& basename) {
  SleepPick p;
  if (basename.empty()) return p;
  p.basename = basename;
  p.fullPath = "/sleep/" + basename;
  p.displayName = FavoriteImage::displayNameForPath(p.fullPath);
  return p;
}

}  // namespace

SleepPick nextSleepFile(const RenderProbe& probe) {
  ensureConfigured();
  if (!probe) return SleepPick{};

  // Paused-rotation branch: re-show the previously rendered wallpaper
  // without advancing the playlist cursor. Mirrors the early-return at
  // renderCustomSleepScreen line 165, hidden inside the facade.
  if (APP_STATE.wallpaperRotationPaused && !APP_STATE.lastSleepWallpaperPath.empty() &&
      Storage.exists(APP_STATE.lastSleepWallpaperPath.c_str())) {
    SleepPick paused;
    paused.fullPath = APP_STATE.lastSleepWallpaperPath;
    paused.displayName = FavoriteImage::displayNameForPath(paused.fullPath);
    paused.isPaused = true;
    if (probe(paused)) {
      return paused;
    }
    // Paused render failed (file vanished mid-flight, parse error, etc.).
    // Fall through to the normal pick path so the user still sees an
    // image, matching the existing renderCustomSleepScreen behaviour
    // where a failed paused render falls into the rotation loop.
  }

  // Sequential-playlist gate: only run the buffer-backed advance()/reconcile()
  // when the heap can truly afford to materialize the order list for THIS
  // folder (measured by real filename bytes — see sequentialPlaylistAffordable);
  // otherwise fall back to the O(1)-heap, anti-repeat direct pick. The old fixed
  // 64 KB gate ignored filename length, so a folder of many long-named .pxc
  // files slipped through and OOM-aborted on lock. Direct pick reads /sleep one
  // entry at a time and never builds a list, so it is safe at any folder size.
  const bool useDirectPick = !sequentialPlaylistAffordable();

  for (int attempt = 0; attempt < kNextSleepFileRetries; ++attempt) {
    std::string basename;
    if (useDirectPick) {
      basename = pickDirectBasename();
    } else {
      // Buffer-backed playlist advance via the facade entry point, so the
      // RFC #145 reconcile notice is drained + persisted here too (a direct
      // v2 advance would skip applyReconcileNotice). advance() can still bail to
      // empty under its internal heap probes — fall through to the direct pick.
      basename = advance();
      if (basename.empty()) {
        basename = pickDirectBasename();
      }
    }
    if (basename.empty()) {
      break;  // /sleep is empty — no point retrying.
    }
    SleepPick pick = makePickFromBasename(basename);
    if (probe(pick)) {
      v2::WallpaperPlaylistV2::instance().rememberRendered(pick.fullPath, pick.basename);
      return pick;
    }
    // Probe rejected this candidate. Loop around — advance() will pick
    // a different file on its next call; direct-pick will re-roll.
  }

  // Root-level fallback ladder: /sleep is empty or every candidate failed to
  // render. PXC takes precedence over BMP (same image, factory waveform, no
  // on-device dithering). Mirrors the tail of the old
  // SleepActivity::renderCustomSleepScreen, hidden inside the facade.
  for (const char* fallbackPath : {"/sleep.pxc", "/sleep_F.bmp", "/sleep.bmp"}) {
    if (!Storage.exists(fallbackPath)) continue;
    SleepPick fb;
    fb.fullPath = fallbackPath;
    fb.displayName = FavoriteImage::displayNameForPath(fallbackPath);
    fb.isFallback = true;
    if (probe(fb)) {
      v2::WallpaperPlaylistV2::instance().rememberRendered(fb.fullPath, "");
      return fb;
    }
  }

  return SleepPick{};
}

void markFolderDirty() {
  ensureConfigured();
  v2::WallpaperPlaylistV2::instance().markFolderDirty();
}

bool reshuffle() {
  ensureConfigured();
  return v2::WallpaperPlaylistV2::instance().reshuffle();
}

void rememberRendered(const std::string& fullPath, const std::string& filename) {
  ensureConfigured();
  v2::WallpaperPlaylistV2::instance().rememberRendered(fullPath, filename);
}

ISleepFs* fs() {
  ensureConfigured();
  return v2::WallpaperPlaylistV2::instance().deps().fs;
}

void reconcileIfDirty() {
  // V2: reconcile is heap-gated and runs lazily inside advance() under the
  // rich-sleep heap-budget gate. No-op here.
}

void Configure(const Config& c) {
  s_configured = true;

  v2::WallpaperPlaylistV2::Deps d;
  d.fs = c.fs;
  d.fileIO = c.fileIO;
  d.orderFilePath = c.orderFilePath;
  d.lastShownFilename = c.lastShownFilename;
  d.lastRenderedPath = c.lastRenderedPath;
  d.saveAppState = c.saveAppState;
  d.randomFn = c.randomFn;
  d.isFavorite = c.isFavorite;
  d.onPathRenamed = c.onPathRenamed;
  d.largestFreeBlockFn = c.largestFreeBlockFn;

  v2::WallpaperPlaylistV2::instance().setDeps(d);
}

void resetForTest() {
  v2::WallpaperPlaylistV2::instance().resetForTest();
  s_configured = false;
}

}  // namespace wallpaper
}  // namespace sleep
}  // namespace crosspoint
