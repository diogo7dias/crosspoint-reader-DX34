#include "SleepActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Paths.h"
#include "activities/reader/ReaderLayoutSafety.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "persist/BackupMirror.h"
#include "util/FavoriteBmp.h"
#include "util/StatusPopup.h"
#include "util/StringUtils.h"
#if SLEEP_V2
#include "sleep/WallpaperPlaylist.h"
#endif

namespace {
void clearLastSleepWallpaperPath() {
  if (!APP_STATE.lastSleepWallpaperPath.empty()) {
    APP_STATE.lastSleepWallpaperPath.clear();
    APP_STATE.saveToFile();
  }
}

void rememberLastRenderedSleepBitmap(const std::string& path, const std::string& sequenceFilename = {}) {
  bool changed = false;
  if (APP_STATE.lastSleepWallpaperPath != path) {
    APP_STATE.lastSleepWallpaperPath = path;
    changed = true;
  }
  if (!sequenceFilename.empty() && APP_STATE.lastShownSleepFilename != sequenceFilename) {
    APP_STATE.lastShownSleepFilename = sequenceFilename;
    changed = true;
  }
  if (changed) {
    APP_STATE.saveToFile();
  }
}

// Returns all .bmp filenames from /sleep in sorted order.
// Does NOT open/validate each file — invalid BMPs are skipped at render time.
std::vector<std::string> getValidSleepBitmaps() {
  std::vector<std::string> files;
  auto dir = Storage.open("/sleep");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return files;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    file.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.') {
      file.close();
      continue;
    }

    if (filename.size() >= 4 && filename.substr(filename.size() - 4) == ".bmp") {
      files.emplace_back(std::move(filename));
    }
    file.close();
    if (files.size() >= CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) break;
  }
  dir.close();

  std::sort(files.begin(), files.end());
  return files;
}

void shuffleSleepPlaylist(std::vector<std::string>& files) {
  if (files.size() <= 1) return;
  for (size_t i = files.size() - 1; i > 0; --i) {
    const auto j = static_cast<size_t>(random(i + 1));
    std::swap(files[i], files[j]);
  }
}

void syncSleepPlaylistWithFiles(const std::vector<std::string>& files, bool forceReshuffle,
                                size_t maxEntries = CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) {
  auto& playlist = APP_STATE.sleepImagePlaylist;
  bool changed = false;

  if (files.empty()) {
    if (!playlist.empty()) {
      playlist.clear();
      APP_STATE.saveToFile();
    }
    return;
  }

  if (forceReshuffle) {
    playlist = files;
    shuffleSleepPlaylist(playlist);
    APP_STATE.lastSleepImage = 0;
    APP_STATE.saveToFile();
    return;
  }

  if (playlist.empty()) {
    // Default behavior: follow stable filename order from /sleep directory.
    playlist = files;
    APP_STATE.lastSleepImage = 0;
    APP_STATE.saveToFile();
    return;
  }

  // files is already sorted; use binary_search for O(log n) membership checks
  // without copying any strings.

  // Remove entries that no longer exist on disk.
  const auto oldSize = playlist.size();
  playlist.erase(
      std::remove_if(playlist.begin(), playlist.end(),
                     [&files](const std::string& e) { return !std::binary_search(files.begin(), files.end(), e); }),
      playlist.end());
  if (playlist.size() != oldSize) {
    changed = true;
  }

  // Find newly-added files not yet in the playlist.
  // Sort a temporary copy of playlist pointers for O(log n) lookup without
  // allocating a separate string_view vector (saves ~1.6 KB for 200 entries).
  // We sort const char* pointers and compare via strcmp — no string copies.
  std::vector<const char*> sortedPtrs;
  sortedPtrs.reserve(playlist.size());
  for (const auto& e : playlist) sortedPtrs.push_back(e.c_str());
  std::sort(sortedPtrs.begin(), sortedPtrs.end(), [](const char* a, const char* b) { return strcmp(a, b) < 0; });

  std::vector<std::string> newFiles;
  std::copy_if(files.begin(), files.end(), std::back_inserter(newFiles), [&sortedPtrs](const std::string& file) {
    return !std::binary_search(sortedPtrs.begin(), sortedPtrs.end(), file.c_str(),
                               [](const char* a, const char* b) { return strcmp(a, b) < 0; });
  });

  // Insert new files right after the current head so they show immediately.
  // When lastSleepImage == 0 nothing has been shown yet, so insert at 0.
  // When lastSleepImage == 1 the head (playlist[0]) is the last-shown image
  // and will be rotated to the back before the next render, so insert at 1.
  if (!newFiles.empty()) {
    const size_t insertPos = (APP_STATE.lastSleepImage == 0) ? 0 : std::min<size_t>(1, playlist.size());
    playlist.insert(playlist.begin() + insertPos, newFiles.begin(), newFiles.end());
    changed = true;
  }

  // Final safety: strictly cap playlist size to avoid serialization OOM.
  if (maxEntries > 0 && playlist.size() > maxEntries) {
    playlist.erase(playlist.begin() + maxEntries, playlist.end());
    changed = true;
  }

  if (playlist.empty()) {
    playlist = files;
    APP_STATE.lastSleepImage = 0;
    changed = true;
  }

  if (changed) {
    APP_STATE.saveToFile();
  }
}

// For large collections (> SLEEP_PLAYLIST_MAX_PERSIST) we do not maintain the
// full playlist in memory. Instead we find the next file after the last-shown
// one using a binary search on the already-sorted files list.
std::string nextSleepImageLargeCollection(const std::vector<std::string>& files) {
  if (files.empty()) {
    return "";
  }

  const auto& last = APP_STATE.lastShownSleepFilename;
  std::string next;

  if (last.empty()) {
    // No prior context: start from the beginning.
    next = files.front();
  } else if (APP_STATE.lastSleepImage == 0) {
    // randomizeSleepImagePlaylist() set a starting file but nothing rendered
    // yet — show it now without advancing past it.
    next = last;
  } else {
    // Find the last-shown file and advance to the next one, wrapping around.
    auto it = std::lower_bound(files.begin(), files.end(), last);
    if (it != files.end() && *it == last) {
      ++it;
    }
    next = (it != files.end()) ? *it : files.front();
  }

  APP_STATE.lastShownSleepFilename = next;
  APP_STATE.lastSleepImage = 1;
  APP_STATE.saveToFile();
  return next;
}

void drawSleepFilenameLabel(const GfxRenderer& renderer, const char* filename) {
  if (!filename || filename[0] == '\0') return;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int safeInset = 18;  // Keep label well inside visible area to avoid bezel clipping.
  const int paddingX = 4;
  const int paddingY = 2;
  const int textLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int maxBoxWidth = std::max(1, screenWidth - safeInset * 2);
  const int maxTextWidth = std::max(1, maxBoxWidth - paddingX * 2 - 2);

  std::string text = renderer.truncatedText(UI_10_FONT_ID, filename, maxTextWidth);
  const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, text.c_str(), EpdFontFamily::REGULAR);
  const int boxWidth = std::min(textWidth + paddingX * 2, maxBoxWidth);
  const int boxHeight = textLineHeight + paddingY * 2;
  const int boxX = safeInset;
  const int boxY = std::max(safeInset, screenHeight - boxHeight - safeInset);
  const int textX = boxX + paddingX;
  const int textY = boxY + paddingY;

  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, true);
  renderer.drawRect(boxX, boxY, boxWidth, boxHeight, false);
  renderer.drawText(UI_10_FONT_ID, textX, textY, text.c_str(), false, EpdFontFamily::REGULAR);
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  // Session boundary: mirror important files to /.crosspoint/backups/ so a
  // later cache-dir wipe or corruption still has a last-resort recovery.
  backup::snapshotAll();

  // Freeze mode keeps the current framebuffer intact — skip the popup
  // so it doesn't get baked into the frozen screen.
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::FREEZE) {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::QUOTES):
      return renderQuotesSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::QUOTES_CUSTOM):
      // Alternate between quotes and custom wallpaper each sleep cycle.
      if (APP_STATE.lastSleepWasQuotes) {
        APP_STATE.lastSleepWasQuotes = false;
        APP_STATE.saveToFile();
        return renderCustomSleepScreen();
      } else {
        APP_STATE.lastSleepWasQuotes = true;
        APP_STATE.saveToFile();
        return renderQuotesSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::FREEZE):
      return renderFreezeSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // When rotation is paused, re-show the same wallpaper without advancing.
  if (APP_STATE.wallpaperRotationPaused && !APP_STATE.lastSleepWallpaperPath.empty() &&
      Storage.exists(APP_STATE.lastSleepWallpaperPath.c_str())) {
    FsFile file;
    if (Storage.openFileForRead("SLP", APP_STATE.lastSleepWallpaperPath, file)) {
      LOG_DBG("SLP", "Paused, re-showing: %s", APP_STATE.lastSleepWallpaperPath.c_str());
      delay(100);
      Bitmap bitmap(file, true);
      const auto parseErr = bitmap.parseHeaders();
      if (parseErr == BmpReaderError::Ok) {
        const std::string displayName = FavoriteBmp::displayNameForPath(APP_STATE.lastSleepWallpaperPath);
        renderBitmapSleepScreen(bitmap, displayName.c_str());
        file.close();
        return;
      }
      LOG_ERR("SLP", "Paused wallpaper parse failed: %s (err=%d)", APP_STATE.lastSleepWallpaperPath.c_str(),
              static_cast<int>(parseErr));
      file.close();
    }
  }

  // Retry advance on parse/open failure so a handful of corrupt BMPs don't
  // waste a sleep render. Each retry calls advance() again, which skips the
  // bad file forward (Large: lex-next; Small: rotates head to tail).
  constexpr int kMaxParseRetries = 5;
  std::string selectedImage;
  bool rendered = false;
  for (int attempt = 0; attempt < kMaxParseRetries && !rendered; ++attempt) {
#if SLEEP_V2
    selectedImage = crosspoint::sleep::WallpaperPlaylist::instance().advance();
#else
    selectedImage.clear();
    {
      const auto files = getValidSleepBitmaps();
      if (!files.empty()) {
        if (files.size() > CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) {
          selectedImage = nextSleepImageLargeCollection(files);
        } else {
          syncSleepPlaylistWithFiles(files, false);
          auto& playlist = APP_STATE.sleepImagePlaylist;
          if (!playlist.empty()) {
            bool changed = false;
            // Rotate head to tail on every pass: first attempt honors the
            // post-render rotation (lastSleepImage != 0); subsequent attempts
            // are explicit skips of the bad file we just tried.
            const bool rotate = (attempt > 0) || (APP_STATE.lastSleepImage != 0 && playlist.size() > 1);
            if (rotate && playlist.size() > 1) {
              const auto first = playlist.front();
              playlist.erase(playlist.begin());
              playlist.push_back(first);
              changed = true;
            }
            selectedImage = playlist.front();
            if (APP_STATE.lastSleepImage != 1) {
              APP_STATE.lastSleepImage = 1;
              changed = true;
            }
            if (changed) APP_STATE.saveToFile();
          }
        }
      }
    }
#endif
    if (selectedImage.empty()) break;
    const auto filename = "/sleep/" + selectedImage;
    FsFile file;
    if (Storage.openFileForRead("SLP", filename, file)) {
      LOG_DBG("SLP", "Loading: %s", filename.c_str());
      delay(100);
      Bitmap bitmap(file, true);
      const auto parseErr = bitmap.parseHeaders();
      if (parseErr == BmpReaderError::Ok) {
        const std::string displayName = FavoriteBmp::displayNameForPath(filename);
        rememberLastRenderedSleepBitmap(filename, selectedImage);
        renderBitmapSleepScreen(bitmap, displayName.c_str());
        file.close();
        rendered = true;
        return;
      }
      LOG_ERR("SLP", "Invalid BMP: %s (err=%d, attempt %d/%d)", filename.c_str(), static_cast<int>(parseErr),
              attempt + 1, kMaxParseRetries);
      file.close();
    } else {
      LOG_ERR("SLP", "Failed to open sleep image: %s (attempt %d/%d)", filename.c_str(), attempt + 1, kMaxParseRetries);
    }
  }

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  for (const char* fallbackPath : {"/sleep_F.bmp", "/sleep.bmp"}) {
    if (Storage.openFileForRead("SLP", fallbackPath, file)) {
      Bitmap bitmap(file, true);
      const auto parseErr = bitmap.parseHeaders();
      if (parseErr == BmpReaderError::Ok) {
        const std::string displayName = FavoriteBmp::displayNameForPath(fallbackPath);
        LOG_DBG("SLP", "Loading: %s", fallbackPath);
        rememberLastRenderedSleepBitmap(fallbackPath);
        renderBitmapSleepScreen(bitmap, displayName.c_str());
        file.close();
        return;
      }
      LOG_ERR("SLP", "Fallback BMP parse failed: %s (err=%d)", fallbackPath, static_cast<int>(parseErr));
      file.close();
    }
  }

  renderDefaultSleepScreen();
}

bool SleepActivity::randomizeSleepImagePlaylist() {
#if SLEEP_V2
  return crosspoint::sleep::WallpaperPlaylist::instance().reshuffle();
#else
  const auto files = getValidSleepBitmaps();
  if (files.empty()) {
    if (!APP_STATE.sleepImagePlaylist.empty()) {
      APP_STATE.sleepImagePlaylist.clear();
      APP_STATE.saveToFile();
    }
    return false;
  }

  if (files.size() > CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST) {
    // Large collection: pick a random starting file; sequential advance
    // resumes from there on the next sleep.
    const auto idx = static_cast<size_t>(random(static_cast<long>(files.size())));
    APP_STATE.sleepImagePlaylist.clear();
    APP_STATE.lastShownSleepFilename = files[idx];
    APP_STATE.lastSleepImage = 0;
    APP_STATE.saveToFile();
    return true;
  }

  syncSleepPlaylistWithFiles(files, true);
  return true;
#endif
}

static size_t s_cachedSleepFavoriteCount = 0;

size_t SleepActivity::cachedSleepFavoriteCount() {
#if SLEEP_V2
  return crosspoint::sleep::WallpaperPlaylist::instance().cachedFavoriteCount();
#else
  return s_cachedSleepFavoriteCount;
#endif
}

void SleepActivity::trimSleepFolderToLimit(GfxRenderer* popupRenderer) {
#if SLEEP_V2
  (void)popupRenderer;  // No caller passes a non-null renderer today.
  crosspoint::sleep::WallpaperPlaylist::instance().trimToLimit();
  return;
#else
  const size_t kLimit = CrossPointState::SLEEP_PLAYLIST_MAX_PERSIST;
  const size_t kScanCap = kLimit + 500;  // Hard cap on scanning to avoid OOM

  // Count .bmp files in /sleep first to avoid allocating the vector if not needed.
  // Also count favorites so callers can use cachedSleepFavoriteCount() without
  // a separate directory scan.
  size_t count = 0;
  size_t scannedFavorites = 0;
  auto dir = Storage.open("/sleep");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    s_cachedSleepFavoriteCount = 0;
    return;
  }
  char name[256];  // Reduced from 500 to save stack
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    std::string filename(name);
    if (!filename.empty() && filename[0] != '.' && filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".bmp") {
      count++;
      if (FavoriteBmp::isFavoritePath("/sleep/" + filename)) {
        scannedFavorites++;
      }
      if (count > kScanCap) break;  // Optimization: we already know we're over limit
    }
    file.close();
  }
  dir.rewindDirectory();

  if (count <= kLimit) {
    s_cachedSleepFavoriteCount = scannedFavorites;
    dir.close();
    return;  // Under limit — nothing to do.
  }

  LOG_INF("SLP", "Trim: /sleep has %zu images (limit %zu), starting prune...", count, kLimit);

  // Now scan and collect files up to cap
  std::vector<std::string> allFiles;
  allFiles.reserve(std::min(count, kScanCap));
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(name, sizeof(name));
    std::string filename(name);
    if (!filename.empty() && filename[0] != '.' && filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".bmp") {
      allFiles.emplace_back(std::move(filename));
      if (allFiles.size() >= kScanCap) {
        file.close();
        break;
      }
    }
    file.close();
  }
  dir.close();

  std::sort(allFiles.begin(), allFiles.end());

  // Sync the playlist against on-disk files: new files are inserted at the
  // front (shown first), deleted files are pruned. Do not cap yet; trim picks
  // the protected favorites to keep first.
  syncSleepPlaylistWithFiles(allFiles, false, 0);

  auto& playlist = APP_STATE.sleepImagePlaylist;
  if (playlist.size() <= kLimit) {
    return;
  }

  const size_t favoriteCount = std::count_if(playlist.begin(), playlist.end(), [](const std::string& filename) {
    return FavoriteBmp::isFavoritePath("/sleep/" + filename);
  });
  s_cachedSleepFavoriteCount = favoriteCount;
  if (favoriteCount > kLimit) {
    LOG_ERR("SLP", "Trim: %zu favorites in /sleep exceed limit %zu", favoriteCount, kLimit);
    return;
  }

  const size_t nonFavoriteBudget = kLimit - favoriteCount;
  size_t keptNonFavorites = 0;
  std::vector<std::string> keep;
  std::vector<std::string> overflow;
  keep.reserve(kLimit);
  overflow.reserve(playlist.size() - kLimit);

  for (const std::string& filename : playlist) {
    const bool isFavorite = FavoriteBmp::isFavoritePath("/sleep/" + filename);
    if (isFavorite) {
      keep.push_back(filename);
      continue;
    }
    if (keptNonFavorites < nonFavoriteBudget) {
      keep.push_back(filename);
      ++keptNonFavorites;
    } else {
      overflow.push_back(filename);
    }
  }

  if (overflow.empty()) {
    if (keep.size() < playlist.size()) {
      playlist = keep;
      APP_STATE.saveToFile();
    }
    return;
  }

  playlist = keep;

  if (popupRenderer) {
    StatusPopup::showBlocking(*popupRenderer, "Moving wallpapers");
  }

  Storage.mkdir("/sleep pause");
  for (const auto& filename : overflow) {
    const std::string src = std::string("/sleep/") + filename;
    const std::string dst = std::string("/sleep pause/") + filename;
    if (!Storage.rename(src.c_str(), dst.c_str())) {
      LOG_ERR("SLP", "Trim: failed to move %s to sleep pause", filename.c_str());
    } else {
      FavoriteBmp::replacePathReferences(src, dst);
      LOG_INF("SLP", "Trim: moved %s to /sleep pause", filename.c_str());
    }
  }
  APP_STATE.saveToFile();
#endif
}

void SleepActivity::renderDefaultSleepScreen() const {
  clearLastSleepWallpaperPath();
  const auto pageHeight = renderer.getScreenHeight();

  // Sleep screen has its own inversion logic; bypass dark mode to avoid
  // double-invert cancellation.
  const bool wasDarkMode = renderer.getDarkMode();
  renderer.setDarkMode(false);

  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, tr(STR_CROSSPOINT), true, EpdFontFamily::REGULAR);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 15, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  renderer.setDarkMode(wasDarkMode);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const char* sourceFilename) const {
  // Sleep screen has its own inversion logic; bypass dark mode to avoid
  // double-invert cancellation.
  const bool wasDarkMode = renderer.getDarkMode();
  renderer.setDarkMode(false);

  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int drawWidth = pageWidth;
  const int drawHeight = pageHeight;
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > drawWidth || bitmap.getHeight() > drawHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(drawWidth) / static_cast<float>(drawHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered
      // vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(drawHeight) - static_cast<float>(drawWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be
      // centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(drawWidth) - static_cast<float>(drawHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (drawWidth - bitmap.getWidth()) / 2;
    y = (drawHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, drawWidth, drawHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (SETTINGS.showSleepImageFilename && sourceFilename != nullptr) {
    drawSleepFilenameLabel(renderer, sourceFilename);
  }

  // Use FULL_REFRESH for the best contrast and cleanest initial state before
  // the grayscale overlay. The extra refresh time is negligible since the
  // device is going to sleep immediately after.
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, drawWidth, drawHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, drawWidth, drawHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  renderer.setDarkMode(wasDarkMode);
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtc") ||
      StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtch")) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, Paths::kDataDir);
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".txt")) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, Paths::kDataDir);
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".epub")) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, Paths::kDataDir);
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      clearLastSleepWallpaperPath();
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  clearLastSleepWallpaperPath();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

// ── Quotes wallpaper ────────────────────────────────────────────────────────

namespace {

/// Parse a _QUOTES.txt file into (chapter, quote) pairs.
struct QuoteEntry {
  std::string chapter;
  std::string text;
};

std::vector<QuoteEntry> parseQuotesFile(const std::string& path) {
  std::vector<QuoteEntry> entries;
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return entries;

  // Read entire file (cap at 8 KB to stay heap-safe)
  constexpr size_t kMaxRead = 8192;
  const size_t fileSize = file.size();
  const size_t readSize = (fileSize < kMaxRead) ? fileSize : kMaxRead;
  std::string buf(readSize, '\0');
  file.read(&buf[0], readSize);
  file.close();

  // Format:  [Chapter Title]\nquote text\n---\n\n
  size_t pos = 0;
  while (pos < buf.size()) {
    // Skip whitespace
    while (pos < buf.size() && (buf[pos] == '\n' || buf[pos] == '\r' || buf[pos] == ' ')) ++pos;
    if (pos >= buf.size()) break;

    QuoteEntry entry;

    // Parse optional [Chapter] header
    if (buf[pos] == '[') {
      auto close = buf.find(']', pos);
      if (close != std::string::npos) {
        entry.chapter = buf.substr(pos + 1, close - pos - 1);
        pos = close + 1;
        // Skip newline after header
        while (pos < buf.size() && (buf[pos] == '\n' || buf[pos] == '\r')) ++pos;
      }
    }

    // Read quote text until --- separator
    auto sep = buf.find("\n---", pos);
    if (sep == std::string::npos) {
      // Last entry without separator
      entry.text = buf.substr(pos);
    } else {
      entry.text = buf.substr(pos, sep - pos);
      pos = sep + 4;  // skip \n---
    }

    // Trim trailing whitespace from quote
    while (!entry.text.empty() && (entry.text.back() == '\n' || entry.text.back() == '\r' || entry.text.back() == ' '))
      entry.text.pop_back();

    if (!entry.text.empty()) entries.push_back(std::move(entry));

    if (sep == std::string::npos) break;
  }
  return entries;
}

/// Scan /recents/ for all _QUOTES.txt files. Returns full paths.
std::vector<std::string> findAllQuotesFiles() {
  std::vector<std::string> result;
  auto dir = Storage.open("/recents");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return result;
  }

  dir.rewindDirectory();
  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string filename(name);
    // Match *_QUOTES.txt
    if (filename.size() > 11 && filename.compare(filename.size() - 11, 11, "_QUOTES.txt") == 0) {
      result.push_back("/recents/" + filename);
    }
    file.close();
  }
  dir.close();
  return result;
}

/// Derive a human-readable book title from a _QUOTES.txt path.
/// E.g. "/recents/My Great Book_QUOTES.txt" → "My Great Book"
std::string bookTitleFromQuotesPath(const std::string& quotesPath) {
  auto slash = quotesPath.rfind('/');
  std::string filename = (slash != std::string::npos) ? quotesPath.substr(slash + 1) : quotesPath;
  // Strip _QUOTES.txt suffix
  const std::string suffix = "_QUOTES.txt";
  if (filename.size() > suffix.size() &&
      filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
    filename = filename.substr(0, filename.size() - suffix.size());
  }
  return filename;
}

}  // namespace

void SleepActivity::renderQuotesSleepScreen() const {
  clearLastSleepWallpaperPath();

  // Find all QUOTES files in /recents/
  auto quotesFiles = findAllQuotesFiles();
  if (quotesFiles.empty()) {
    LOG_DBG("SLP", "No quotes files found in /recents/");
    return renderDefaultSleepScreen();
  }

  // Pick a random quotes file
  const std::string& chosenFile = quotesFiles[random(static_cast<long>(quotesFiles.size()))];

  auto quotes = parseQuotesFile(chosenFile);
  if (quotes.empty()) {
    return renderDefaultSleepScreen();
  }

  // Pick a random quote from that file
  const auto& entry = quotes[random(static_cast<long>(quotes.size()))];

  // Derive book title from the quotes filename
  const std::string bookTitle = bookTitleFromQuotesPath(chosenFile);

  // --- Render notebook-style wallpaper ---
  const bool wasDarkMode = renderer.getDarkMode();
  renderer.setDarkMode(false);

  const int W = renderer.getScreenWidth();   // 480
  const int H = renderer.getScreenHeight();  // 800

  // White background
  renderer.clearScreen(0xFF);

  // Layout constants
  const int marginX = 20;
  const int contentWidth = W - marginX * 2;
  const int headerTopY = 40;

  // ── Header: book title (bold, centered) ──
  const int titleFontId = UI_12_FONT_ID;
  const int chapterFontId = UI_10_FONT_ID;

  // Wrap and draw book title
  auto titleLines = ReaderLayoutSafety::wrapText(renderer, titleFontId, bookTitle, contentWidth);
  if (titleLines.size() > 2) titleLines.resize(2);  // max 2 lines for title

  int y = headerTopY;
  for (const auto& line : titleLines) {
    const int textW = renderer.getTextWidth(titleFontId, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(titleFontId, (W - textW) / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(titleFontId) + 2;
  }

  // ── Chapter name (italic, centered, smaller — wrapped, not truncated) ──
  if (!entry.chapter.empty()) {
    y += 4;
    auto chapterLines = ReaderLayoutSafety::wrapText(renderer, chapterFontId, entry.chapter, contentWidth);
    if (chapterLines.size() > 2) chapterLines.resize(2);  // max 2 lines
    for (const auto& chLine : chapterLines) {
      const int chW = renderer.getTextWidth(chapterFontId, chLine.c_str(), EpdFontFamily::ITALIC);
      renderer.drawText(chapterFontId, (W - chW) / 2, y, chLine.c_str(), true, EpdFontFamily::ITALIC);
      y += renderer.getLineHeight(chapterFontId) + 2;
    }
    y += 2;
  }

  // ── Divider line — edge-to-edge, 3px thick ──
  y += 8;
  renderer.fillRect(0, y, W, 3, true);
  y += 10;  // margin below divider

  // ── Quote text — vertically centered in remaining space ──
  const int quoteTopY = y;
  const int quoteBottomY = H;  // use full bottom
  const int quoteAreaH = quoteBottomY - quoteTopY;
  const int lineSpacing = 28;

  // Determine quote font: use smaller font if quote is long
  int quoteFontId = CHAREINK_14_FONT_ID;
  {
    auto testLines = ReaderLayoutSafety::wrapText(renderer, quoteFontId, entry.text, contentWidth);
    const int maxLines = quoteAreaH / lineSpacing;
    if (static_cast<int>(testLines.size()) > maxLines) {
      quoteFontId = UI_12_FONT_ID;
    }
  }

  auto quoteLines = ReaderLayoutSafety::wrapText(renderer, quoteFontId, entry.text, contentWidth);

  // Cap to lines that fit
  const int maxQuoteLines = quoteAreaH / lineSpacing;
  if (static_cast<int>(quoteLines.size()) > maxQuoteLines) {
    quoteLines.resize(maxQuoteLines);
    if (!quoteLines.empty()) {
      auto& last = quoteLines.back();
      last = renderer.truncatedText(quoteFontId, last.c_str(), contentWidth, EpdFontFamily::REGULAR);
    }
  }

  // Vertically center the block of quote lines in the available space
  const int totalTextH = static_cast<int>(quoteLines.size()) * lineSpacing;
  int quoteY = quoteTopY + (quoteAreaH - totalTextH) / 2;

  for (const auto& line : quoteLines) {
    const int lineW = renderer.getTextWidth(quoteFontId, line.c_str(), EpdFontFamily::REGULAR);
    const int lineX = marginX + (contentWidth - lineW) / 2;
    renderer.drawText(quoteFontId, lineX, quoteY, line.c_str(), true, EpdFontFamily::REGULAR);
    quoteY += lineSpacing;
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  renderer.setDarkMode(wasDarkMode);
}

void SleepActivity::renderFreezeSleepScreen() const {
  clearLastSleepWallpaperPath();

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();

  // The framebuffer still contains the last render. Draw a small black
  // pill at the bottom center with "Sleeping" label so the user knows
  // the device is locked.

  const int fontId = UI_10_FONT_ID;
  const char* label = "SLEEPING.";
  const int textW = renderer.getTextWidth(fontId, label, EpdFontFamily::REGULAR);
  const int textH = renderer.getLineHeight(fontId);
  const int padY = 11;
  const int bannerH = textH + padY * 2;
  // Extend banner beyond screen edges so offset displays still show full black
  const int bannerX = -20;
  const int bannerW = W + 40;
  const int bannerY = H - bannerH - 10;

  // Full-width black banner
  renderer.fillRect(bannerX, bannerY, bannerW, bannerH, true);
  // Centered white text
  const int textX = (W - textW) / 2;
  renderer.drawText(fontId, textX, bannerY + padY, label, false, EpdFontFamily::REGULAR);

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
