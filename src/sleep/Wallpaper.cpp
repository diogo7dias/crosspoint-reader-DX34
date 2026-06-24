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
#include "SleepMoveSelection.h"
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
  const auto& deps = v2::WallpaperPlaylistV2::instance().deps();
  auto* sfs = deps.fs;
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

  // Consolidated heap probe (RFC: sleep heap-gate unification): read the SAME
  // largest-free-block source the V2 playlist's inner gates use
  // (deps.largestFreeBlockFn) instead of calling the global util directly. The
  // two gates previously consulted two different injection points, so a host
  // test scripting heap fragmentation moved one but not the other and the outer
  // gate silently saw the real device heap. Routing both through one injected
  // probe makes the whole sequential-vs-direct decision drivable from a single
  // fake. When the probe is unset (older Configure path) fall back to the global
  // util, which carries its own host override. Total-free keeps its own util
  // (it has a separate host override and no per-alloc equivalent in V2).
  const size_t largestFree =
      deps.largestFreeBlockFn ? deps.largestFreeBlockFn() : crosspoint::heap::largestFreeBlockBytes();

  return largestFree >= contigNeed && crosspoint::mem::totalFreeBytes() >= totalNeed;
}

// Streaming fragment-safe pick — O(1) heap beyond the returned basename.
// Empty if /sleep is empty or fs not wired.
//
// DETERMINISTIC SEQUENTIAL: returns the lexicographically next .bmp/.pxc
// strictly after `after`, wrapping to the lex-first at the end of the lap.
// `after` is the just-shown basename (lastShownSleepFilename), persisted across
// deep sleep, so successive wakes walk the folder one file at a time in a fixed
// order — never random, never repeating mid-lap. This is the low-heap fallback
// for when the mtime-ordered buffer playlist can't be materialized; it trades
// new-on-top ordering (which needs a heap-resident sorted buffer) for a stable
// alphabetical march that holds at any folder size and any fragmentation level.
// Earlier builds rolled ::random() here, which is exactly what produced the
// "random wallpapers + same one too often" the user reported whenever the heap
// gate sent rotation down this path.
std::string pickDirectBasename(const std::string& after) {
  auto* sfs = v2::WallpaperPlaylistV2::instance().deps().fs;
  if (!sfs) return {};
  return sfs->nextSleepBmpAfter(after);
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

  // Sequential cursor for the direct-pick path: starts at the just-shown file
  // so the first pick is the one after it, and advances past any candidate the
  // probe rejects so a bad/corrupt file doesn't get re-picked every retry
  // (pickDirectBasename is deterministic — same `after` returns the same file).
  std::string directAfter = APP_STATE.lastShownSleepFilename;

  for (int attempt = 0; attempt < kNextSleepFileRetries; ++attempt) {
    std::string basename;
    if (useDirectPick) {
      basename = pickDirectBasename(directAfter);
    } else {
      // Buffer-backed playlist advance via the facade entry point, so the
      // RFC #145 reconcile notice is drained + persisted here too (a direct
      // v2 advance would skip applyReconcileNotice). advance() can still bail to
      // empty under its internal heap probes — fall through to the direct pick.
      basename = advance();
      if (basename.empty()) {
        basename = pickDirectBasename(directAfter);
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
    // Probe rejected this candidate. advance() picks a different file on its
    // next call; for direct-pick, step the cursor past the rejected basename so
    // the next retry returns the following file in sequence, not the same one.
    directAfter = basename;
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

size_t countImages(size_t scanCap) {
  ensureConfigured();
  ISleepFs* sfs = v2::WallpaperPlaylistV2::instance().deps().fs;
  return sfs ? sfs->countSleepBmps(scanCap) : 0;
}

size_t moveRandomToPause(size_t n) {
  ensureConfigured();
  if (n == 0) return 0;
  const auto& deps = v2::WallpaperPlaylistV2::instance().deps();
  ISleepFs* sfs = deps.fs;
  if (!sfs) return 0;

  // No usable randomness → fall back to a deterministic "keep first n" pick so
  // the action still works (production always wires deps.randomFn).
  RandomFn rnd = deps.randomFn ? deps.randomFn : [](long) -> long { return 0; };

  // Stream /sleep once, reservoir-sampling n names — never materializes the
  // whole folder. `name` points at the SD layer's stack buffer; copy it in.
  Reservoir reservoir(n, rnd);
  sfs->walkSleepBmps(
      [&reservoir](const char* name, size_t len, uint32_t /*mtime*/) { reservoir.offer(std::string(name, len)); });

  const std::vector<std::string>& picks = reservoir.take();
  if (picks.empty()) return 0;

  sfs->mkdir("/sleep pause");
  size_t moved = 0;
  for (const auto& nm : picks) {
    const std::string from = "/sleep/" + nm;
    const std::string to = "/sleep pause/" + nm;
    if (sfs->rename(from, to)) {
      if (deps.onPathRenamed) deps.onPathRenamed(from, to);
      ++moved;
    }
  }
  if (moved > 0) v2::WallpaperPlaylistV2::instance().markFolderDirty();
  return moved;
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
