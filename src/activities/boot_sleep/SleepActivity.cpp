#include "SleepActivity.h"

#include <Epub.h>
#include <Epub/converters/DirectPixelWriter.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Paths.h"
#include "activities/reader/ReaderLayoutSafety.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "persist/BackupMirror.h"
#include "persist/PersistManager.h"
#include "sleep/WallpaperPlaylist.h"
#if FEATURE_WALLPAPER_V2
#include "sleep/WallpaperPlaylistV2.h"
#endif
#include "util/FavoriteBmp.h"
#include "util/StringUtils.h"

namespace {
// Sleep rendering runs inside SleepActivity::onEnter, called AFTER
// enterDeepSleep's persistAppState flushAll and milliseconds before the CPU
// enters deep sleep. saveToFile() is debounced — the next main-loop tick
// never fires, so any state write would be lost. Force a sync flush so
// triage state survives the deep-sleep boundary.
void flushStateSync() {
  APP_STATE.saveToFile();
  crosspoint::persist::PersistManager().flushAll();
}

void clearLastSleepWallpaperPath() {
  if (!APP_STATE.lastSleepWallpaperPath.empty()) {
    APP_STATE.lastSleepWallpaperPath.clear();
    flushStateSync();
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
    flushStateSync();
  }
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

  // Note: no "Entering sleep..." popup here — main.cpp already drew the
  // "Going to sleep..." banner on the power-button path, and a second
  // popup on top of it was read as duplicate noise.

  // Heap-budget gate: weak-WiFi sessions have tipped the device into abort()
  // during sleep wallpaper / cover render with Min Free observed at 4352 bytes.
  // If the heap is already pressured when sleep is entered, fall back to a
  // blank sleep screen — render fragments + bitmap parsing on top of a tight
  // heap is the documented crash signature.
  constexpr uint32_t kMinFreeHeapForRichSleep = 30 * 1024;
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < kMinFreeHeapForRichSleep) {
    LOG_DBG("SLP", "Free heap %u < %u, suppressing wallpaper/cover render", freeHeap, kMinFreeHeapForRichSleep);
    return renderBlankSleepScreen();
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
    if (StringUtils::checkFileExtension(APP_STATE.lastSleepWallpaperPath, ".pxc")) {
      LOG_DBG("SLP", "Paused, re-showing PXC: %s", APP_STATE.lastSleepWallpaperPath.c_str());
      delay(100);
      const std::string displayName = FavoriteBmp::displayNameForPath(APP_STATE.lastSleepWallpaperPath);
      if (renderPxcSleepScreen(APP_STATE.lastSleepWallpaperPath, displayName.c_str())) {
        return;
      }
      LOG_ERR("SLP", "Paused PXC render failed: %s", APP_STATE.lastSleepWallpaperPath.c_str());
    } else {
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
  }

  // Retry advance on parse/open failure so a handful of corrupt BMPs don't
  // waste a sleep render. Each retry calls advance() again, which skips the
  // bad file forward (Large: lex-next; Small: rotates head to tail).
  constexpr int kMaxParseRetries = 5;
  std::string selectedImage;
  bool rendered = false;
  for (int attempt = 0; attempt < kMaxParseRetries && !rendered; ++attempt) {
#if FEATURE_WALLPAPER_V2
    selectedImage = crosspoint::sleep::v2::WallpaperPlaylistV2::instance().advance();
#else
    selectedImage = crosspoint::sleep::WallpaperPlaylist::instance().advance();
#endif
    if (selectedImage.empty()) break;
    const auto filename = "/sleep/" + selectedImage;
    if (StringUtils::checkFileExtension(selectedImage, ".pxc")) {
      LOG_DBG("SLP", "Loading PXC: %s", filename.c_str());
      delay(100);
      const std::string displayName = FavoriteBmp::displayNameForPath(filename);
      if (renderPxcSleepScreen(filename, displayName.c_str())) {
        rememberLastRenderedSleepBitmap(filename, selectedImage);
        rendered = true;
        return;
      }
      LOG_ERR("SLP", "PXC render failed: %s (attempt %d/%d)", filename.c_str(), attempt + 1, kMaxParseRetries);
      continue;
    }
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

  // Root-level fallback wallpapers. PXC takes precedence over BMP — same image,
  // PXC renders without on-device dithering and with the factory waveform.
  if (Storage.exists("/sleep.pxc")) {
    LOG_DBG("SLP", "Loading: /sleep.pxc");
    if (renderPxcSleepScreen("/sleep.pxc")) {
      rememberLastRenderedSleepBitmap("/sleep.pxc");
      return;
    }
    LOG_ERR("SLP", "Root /sleep.pxc render failed");
  }
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
#if FEATURE_WALLPAPER_V2
  return crosspoint::sleep::v2::WallpaperPlaylistV2::instance().reshuffle();
#else
  return crosspoint::sleep::WallpaperPlaylist::instance().reshuffle();
#endif
}

size_t SleepActivity::cachedSleepFavoriteCount() {
#if FEATURE_WALLPAPER_V2
  // PR2: V2 will track cached favorite count via its trim path.
  return 0;
#else
  return crosspoint::sleep::WallpaperPlaylist::instance().cachedFavoriteCount();
#endif
}

void SleepActivity::trimSleepFolderToLimit(GfxRenderer* popupRenderer) {
  (void)popupRenderer;  // No caller passes a non-null renderer today.
#if FEATURE_WALLPAPER_V2
  // V2: only mark dirty. Calling reconcile() inline here (e.g. from
  // MyLibraryActivity move-to-sleep) runs on the file-browser heap which is
  // fragmented enough that the buffer_.insert() string growth (~28 KB
  // observed allocation fail) throws bad_alloc → terminate. Reconcile fires
  // safely on next sleep entry from advance(), under the rich-sleep heap-
  // budget gate (30 KB free required). Net effect: file moved to /sleep is
  // visible on the user's next sleep cycle, which is the natural moment
  // they care about.
  crosspoint::sleep::v2::WallpaperPlaylistV2::instance().markFolderDirty();
#else
  crosspoint::sleep::WallpaperPlaylist::instance().trimToLimit();
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
    const auto mode =
        SETTINGS.useFactoryLUT ? GfxRenderer::GrayscaleMode::FactoryQuality : GfxRenderer::GrayscaleMode::Differential;
    renderer.renderGrayscale(mode, [&]() {
      bitmap.rewindToData();
      renderer.drawBitmap(bitmap, x, y, drawWidth, drawHeight, cropX, cropY);
    });
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

bool SleepActivity::renderPxcSleepScreen(const std::string& path, const char* sourceFilename) const {
  // PXC layout: uint16 width, uint16 height, then packed 2bpp payload (4 px/byte, MSB first).
  // Pixel convention: 0=Black, 1=DarkGray, 2=LightGray, 3=White — same as Bitmap::readNextRow.
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    LOG_ERR("SLP", "Cannot open PXC: %s", path.c_str());
    return false;
  }
  uint16_t pxcWidth = 0, pxcHeight = 0;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("SLP", "PXC header read failed: %s", path.c_str());
    file.close();
    return false;
  }
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  if (std::abs(static_cast<int>(pxcWidth) - sw) > 1 || std::abs(static_cast<int>(pxcHeight) - sh) > 1) {
    LOG_ERR("SLP", "PXC size mismatch %dx%d (screen %dx%d): %s", pxcWidth, pxcHeight, sw, sh, path.c_str());
    file.close();
    return false;
  }
  const uint32_t dataOffset = file.position();
  const int bytesPerRow = (pxcWidth + 3) / 4;
  uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!rowBuf) {
    LOG_ERR("SLP", "PXC row alloc failed (%d bytes)", bytesPerRow);
    file.close();
    return false;
  }

  const auto mode =
      SETTINGS.useFactoryLUT ? GfxRenderer::GrayscaleMode::FactoryQuality : GfxRenderer::GrayscaleMode::Differential;
  const int width = static_cast<int>(pxcWidth);
  const int height = static_cast<int>(pxcHeight);
  renderer.renderGrayscale(mode, [&]() {
    file.seek(dataOffset);
    DirectPixelWriter pw;
    pw.init(renderer);
    for (int row = 0; row < height; row++) {
      if (file.read(rowBuf, bytesPerRow) != bytesPerRow) break;
      pw.beginRow(row);
      for (int col = 0; col < width; col++) {
        const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
        pw.writePixel(col, pv);
      }
      if ((row & 31) == 0) esp_task_wdt_reset();
    }
  });

  free(rowBuf);
  file.close();

  if (sourceFilename != nullptr && SETTINGS.showSleepImageFilename) {
    drawSleepFilenameLabel(renderer, sourceFilename);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  return true;
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

  // The framebuffer still contains the last render. Leave it untouched —
  // the "Going to sleep..." toast drawn by main.cpp on power-button sleep
  // is the user-facing sleep indicator, so no pill/banner is needed here.
  // Still fire a HALF_REFRESH so the panel latches the final frame cleanly.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
