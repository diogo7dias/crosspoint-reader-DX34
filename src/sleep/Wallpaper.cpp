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
  d.onTrimMoved = [](uint16_t /*moved*/) {};
  d.onFavoritesCapBlocked = []() {};
  // Heap probe (RFC #156 C2): inject the device's contiguous-block query
  // through the playlist so the same code path runs under host tests with
  // a scripted heap. UNIT_TEST_HOST builds don't link esp_heap_caps; the
  // playlist treats nullptr as "unlimited heap".
#ifndef UNIT_TEST_HOST
  d.largestFreeBlockFn = []() { return heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT); };
#endif

  v2::WallpaperPlaylistV2::instance().setDeps(d);
}

}  // namespace

std::string advance() {
  ensureConfigured();
  return v2::WallpaperPlaylistV2::instance().advance();
}

namespace {

// Retry budget for nextSleepFile's probe loop. Mirrors the
// kMaxParseRetries constant currently in SleepActivity. Until C4 of
// RFC #156 lands, both copies coexist.
constexpr int kNextSleepFileRetries = 5;

// Heap-fragmentation gate. Mirrors kSleepLargestBlockSafeBytes in
// SleepActivity; copy lives here so the facade owns the decision when
// callers migrate. C2 of RFC #156 replaces this with an injected lambda
// so host tests can simulate fragmentation; today the host build always
// reports unlimited heap (preserves existing test invariants).
constexpr size_t kNextSleepFileHeapGateBytes = 64 * 1024;

size_t largestFreeBlockBytes() {
  // Route through the playlist's injected probe so production + host tests
  // see the same value. ensureConfigured() (called by nextSleepFile before
  // this is invoked) wires the production lambda; tests inject their own
  // via Configure(). nullptr → assume unlimited (matches pre-C2 host).
  const auto& fn = v2::WallpaperPlaylistV2::instance().deps().largestFreeBlockFn;
  return fn ? fn() : SIZE_MAX;
}

// Streaming fragment-safe pick — direct mirror of SleepActivity's
// pickSleepFileDirect helper. Touches O(1) heap beyond the returned
// basename. Empty if /sleep is empty or fs not wired.
std::string pickDirectBasename() {
  auto* sfs = v2::WallpaperPlaylistV2::instance().deps().fs;
  if (!sfs) return {};
  const size_t count = sfs->countSleepBmps(kSleepFolderCap);
  if (count == 0) return {};
  const size_t idx = static_cast<size_t>(::random(static_cast<long>(count)));
  return sfs->nthSleepBmp(idx);
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

  const bool useDirectPick = largestFreeBlockBytes() < kNextSleepFileHeapGateBytes;

  for (int attempt = 0; attempt < kNextSleepFileRetries; ++attempt) {
    std::string basename;
    if (useDirectPick) {
      basename = pickDirectBasename();
    } else {
      // Buffer-backed playlist advance. Internal heap probes (see
      // WallpaperPlaylistV2::reconcile / writeBuffer) bail to empty if
      // the contiguous block is too small — we then fall through to the
      // streaming direct pick on the same iteration.
      basename = v2::WallpaperPlaylistV2::instance().advance();
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
  d.onTrimMoved = c.onTrimMoved;
  d.onFavoritesCapBlocked = c.onFavoritesCapBlocked;
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
