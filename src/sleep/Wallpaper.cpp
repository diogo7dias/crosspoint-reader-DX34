#include "Wallpaper.h"

#include <Arduino.h>

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
  d.fs                = &defaultFs();
  d.fileIO            = &defaultFileIO();
  d.lastShownFilename = &APP_STATE.lastShownSleepFilename;
  d.lastRenderedPath  = &APP_STATE.lastSleepWallpaperPath;
  // advance() runs inside SleepActivity::onEnter, milliseconds before the
  // CPU enters deep sleep. The PersistManager debounce window never fires
  // before sleep, so we force a synchronous flush so rotation state
  // survives the deep-sleep boundary.
  d.saveAppState = []() {
    const bool ok = APP_STATE.saveToFile();
    crosspoint::persist::PersistManager().flushAll();
    return ok;
  };
  d.randomFn      = [](long mod) -> long { return ::random(mod); };
  d.isFavorite    = [](const std::string& path) { return FavoriteImage::isFavoritePath(path); };
  d.onPathRenamed = [](const std::string& from, const std::string& to) {
    FavoriteImage::replacePathReferences(from, to);
  };
  d.onTrimMoved           = [](uint16_t /*moved*/) {};
  d.onFavoritesCapBlocked = []() {};

  v2::WallpaperPlaylistV2::instance().setDeps(d);
}

}  // namespace

std::string advance() {
  ensureConfigured();
  return v2::WallpaperPlaylistV2::instance().advance();
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
  d.fs                    = c.fs;
  d.fileIO                = c.fileIO;
  d.orderFilePath         = c.orderFilePath;
  d.lastShownFilename     = c.lastShownFilename;
  d.lastRenderedPath      = c.lastRenderedPath;
  d.saveAppState          = c.saveAppState;
  d.randomFn              = c.randomFn;
  d.isFavorite            = c.isFavorite;
  d.onPathRenamed         = c.onPathRenamed;
  d.onTrimMoved           = c.onTrimMoved;
  d.onFavoritesCapBlocked = c.onFavoritesCapBlocked;

  v2::WallpaperPlaylistV2::instance().setDeps(d);
}

void resetForTest() {
  v2::WallpaperPlaylistV2::instance().resetForTest();
  s_configured = false;
}

}  // namespace wallpaper
}  // namespace sleep
}  // namespace crosspoint
