/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <EpdFontFamily.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include <memory>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/themes/BaseTheme.h"
#include "persist/BackupMirror.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long confirmDoubleTapMs = 350;
constexpr unsigned long progressSaveDebounceMs = 800;
constexpr int recentSwitcherRows = 8;
}  // namespace

void XtcReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!xtc) {
    return;
  }

  EpdFontFamily::setReaderBoldSwapEnabled(SETTINGS.readerBoldSwap != 0);
  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  // Generate cover thumbnail for home screen cover layouts
  xtc->generateThumbBmp(400);

  // Move book to /recents/ folder on first open from another location
  {
    std::string newPath = RECENT_BOOKS.moveBookToRecents(xtc->getPath());
    if (!newPath.empty()) {
      xtc->setPath(newPath);
    }
  }

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  flushProgressIfNeeded(true);
  pendingMenuOpen = false;
  EpdFontFamily::setReaderBoldSwapEnabled(false);
  ActivityWithSubactivity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  flushProgressIfNeeded(false);

  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    confirmLongPressHandled = false;
  }

  if (pendingMenuOpen && !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      millis() - lastConfirmReleaseMs > confirmDoubleTapMs) {
    pendingMenuOpen = false;
    openChapterMenu();
    return;
  }

  if (recentSwitcherOpen) {
    const bool prevTriggered = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                               mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool nextTriggered = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                               mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (prevTriggered && !recentSwitcherBooks.empty()) {
      recentSwitcherSelection =
          (recentSwitcherSelection + static_cast<int>(recentSwitcherBooks.size()) - 1) % recentSwitcherBooks.size();
      requestUpdate();
      return;
    }
    if (nextTriggered && !recentSwitcherBooks.empty()) {
      recentSwitcherSelection = (recentSwitcherSelection + 1) % recentSwitcherBooks.size();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !recentSwitcherBooks.empty()) {
      const std::string selectedPath = recentSwitcherBooks[recentSwitcherSelection].path;
      recentSwitcherOpen = false;
      if (!selectedPath.empty()) {
        onOpenBook(selectedPath);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
      recentSwitcherOpen = false;
      requestUpdate();
      return;
    }
    return;
  }

  // Single tap opens chapter menu; double tap toggles text render mode (Dark/Crisp).
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const unsigned long now = millis();
    if (pendingMenuOpen && now - lastConfirmReleaseMs <= confirmDoubleTapMs) {
      pendingMenuOpen = false;
      toggleTextRenderMode();
      return;
    }
    pendingMenuOpen = true;
    lastConfirmReleaseMs = now;
    return;
  }

  // Long press CONFIRM (1s+) toggles orientation: Portrait <-> Landscape CCW.
  if (!confirmLongPressHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= goHomeMs) {
    confirmLongPressHandled = true;
    mappedInput.suppressUntilAllReleased();
    SETTINGS.orientation = (SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW)
                               ? CrossPointSettings::ORIENTATION::PORTRAIT
                               : CrossPointSettings::ORIENTATION::LANDSCAPE_CCW;
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("XRS", "Failed to save settings after orientation change");
    }
    renderer.setOrientation(SETTINGS.orientation == CrossPointSettings::ORIENTATION::LANDSCAPE_CCW
                                ? GfxRenderer::Orientation::LandscapeCounterClockwise
                                : GfxRenderer::Orientation::Portrait);
    requestUpdate();
    return;
  }

  // BACK: go home immediately on press for snappier response.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of
  // release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool prevTriggered = usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasPressed(MappedInputManager::Button::Left))
                                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                                    mappedInput.wasReleased(MappedInputManager::Button::Left));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered = usePressForPageTurn
                                 ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasPressed(MappedInputManager::Button::Right))
                                 : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) || powerPageTurn ||
                                    mappedInput.wasReleased(MappedInputManager::Button::Right));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  const bool skipPages = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    progressDirty = true;
    lastProgressChangeMs = millis();
    flushProgressIfNeeded(true);
    requestUpdate();
  } else if (nextTriggered) {
    const uint32_t previousPage = currentPage;
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    if (currentPage > previousPage) {
      APP_STATE.sessionPagesRead += currentPage - previousPage;
    }
    progressDirty = true;
    lastProgressChangeMs = millis();
    flushProgressIfNeeded(true);
    requestUpdate();
  }
}

void XtcReaderActivity::openChapterMenu() {
  if (!xtc || !xtc->hasChapters() || xtc->getChapters().empty()) {
    return;
  }
  exitActivity();
  enterNewActivity(new XtcReaderChapterSelectionActivity(
      this->renderer, this->mappedInput, xtc, currentPage,
      [this] {
        exitActivity();
        requestUpdate();
      },
      [this](const uint32_t newPage) {
        currentPage = newPage;
        progressDirty = true;
        lastProgressChangeMs = millis();
        flushProgressIfNeeded(true);
        exitActivity();
        requestUpdate();
      }));
}

void XtcReaderActivity::toggleTextRenderMode() {
  flushProgressIfNeeded(true);
  SETTINGS.textRenderMode = (SETTINGS.textRenderMode + 1) % CrossPointSettings::TEXT_RENDER_MODE_COUNT;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("XRS", "Failed to save settings after text render mode toggle");
  }
  requestUpdate();
}

void XtcReaderActivity::render(Activity::RenderLock&&) {
  if (!xtc) {
    return;
  }

  if (recentSwitcherOpen) {
    renderRecentSwitcher();
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  if (lastObservedPage != static_cast<int32_t>(currentPage)) {
    lastObservedPage = static_cast<int32_t>(currentPage);
    if (lastSavedPage != static_cast<int32_t>(currentPage)) {
      progressDirty = true;
      lastProgressChangeMs = millis();
    }
  }
  flushProgressIfNeeded(false);
}

void XtcReaderActivity::renderRecentSwitcher() {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int popupX = 18;
  const int popupY = 24;
  const int popupW = screenW - popupX * 2;
  const int popupH = screenH - popupY * 2;
  const int titleY = popupY + 8;
  const int rowsY = popupY + 30;
  const int rowsH = popupH - 40;
  const int rowH = rowsH / recentSwitcherRows;

  renderer.clearScreen();
  renderer.drawRect(popupX, popupY, popupW, popupH, true);
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_MENU_RECENT_BOOKS), true, EpdFontFamily::REGULAR);

  for (int i = 0; i < recentSwitcherRows; i++) {
    const int rowY = rowsY + i * rowH;
    const bool hasBook = i < static_cast<int>(recentSwitcherBooks.size());
    const bool selected = hasBook && i == recentSwitcherSelection;

    if (selected) {
      renderer.fillRect(popupX + 8, rowY, popupW - 16, rowH - 2, true);
      renderer.drawRect(popupX + 10, rowY + 2, popupW - 20, rowH - 6, false);
    } else {
      renderer.drawRect(popupX + 8, rowY, popupW - 16, rowH - 2, true);
    }

    std::string title = " ";
    if (hasBook) {
      title = recentSwitcherBooks[i].title;
      if (title.empty()) {
        const size_t lastSlash = recentSwitcherBooks[i].path.find_last_of('/');
        title = (lastSlash == std::string::npos) ? recentSwitcherBooks[i].path
                                                 : recentSwitcherBooks[i].path.substr(lastSlash + 1);
      }
      title = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), popupW - 28);
    }
    renderer.drawText(UI_10_FONT_ID, popupX + 14, rowY + 3, title.c_str(), !selected);
  }

  renderer.displayBuffer();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Sanity-check dimensions before computing buffer size.
  // Real e-ink pages are at most ~1024x1404; anything beyond 4096 is corrupt data.
  // This also prevents integer overflow in the size_t multiplication below.
  if (pageWidth == 0 || pageHeight == 0 || pageWidth > 4096 || pageHeight > 4096) {
    LOG_ERR("XTR", "Invalid XTC page dimensions: %ux%u", pageWidth, pageHeight);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2
  // bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  auto pageBuffer = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[pageBufferSize]);
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer.get(), pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu", currentPage);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full
  // page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer.get();              // Bit1 plane
    const uint8_t* plane2 = pageBuffer.get() + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;          // Bytes per column (100 for 800 height)

    // Lambda to get raw pixel value at (x, y)
    auto getRawPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Apply contrast boost: remap grayscale values
    // Normal: 0=W, 1=DkGray, 2=LtGray, 3=Black (unchanged)
    // High:   0=W, 1=Black,  2=DkGray, 3=Black (boost one step)
    // Max:    0=W, 1=Black,  2=Black,  3=Black (all non-white → black)
    const uint8_t contrast = SETTINGS.xtcContrast;
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const uint8_t raw = getRawPixelValue(x, y);
      if (contrast == CrossPointSettings::XTC_CONTRAST_MAX) {
        return raw >= 1 ? 3 : 0;  // all non-white → black
      }
      if (contrast == CrossPointSettings::XTC_CONTRAST_HIGH) {
        // Boost one level: 1→3(black), 2→1(dark gray)
        static constexpr uint8_t highMap[4] = {0, 3, 1, 3};
        return highMap[raw];
      }
      return raw;
    };

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
    esp_task_wdt_reset();

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    // Max contrast: all non-white already mapped to black, skip grayscale passes
    if (contrast == CrossPointSettings::XTC_CONTRAST_MAX) {
      LOG_DBG("XTR", "Rendered page %lu/%lu (max contrast, 1-bit)", currentPage + 1, xtc->getPageCount());
      return;
    }

    // Grayscale passes (Normal & High contrast modes)

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    esp_task_wdt_reset();
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    esp_task_wdt_reset();
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer for next frame
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
    esp_task_wdt_reset();

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit, contrast=%u)", currentPage + 1, xtc->getPageCount(), contrast);
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  const std::string progPath = xtc->getCachePath() + "/progress.bin";
  const std::string tmpPath = xtc->getCachePath() + "/progress_tmp.bin";
  const std::string bakPath = xtc->getCachePath() + "/progress.bin.bak";

  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  FsFile f;
  if (Storage.openFileForWrite("XTR", tmpPath.c_str(), f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();

    // Rotate current progress.bin to progress.bin.bak before replacing.
    if (Storage.exists(progPath.c_str())) {
      if (Storage.exists(bakPath.c_str())) {
        Storage.remove(bakPath.c_str());
      }
      Storage.rename(progPath.c_str(), bakPath.c_str());
    }
    Storage.rename(tmpPath.c_str(), progPath.c_str());
  }
}

void XtcReaderActivity::flushProgressIfNeeded(const bool force) {
  if (!xtc || !progressDirty) {
    return;
  }

  const auto now = millis();
  if (!force && now - lastProgressChangeMs < progressSaveDebounceMs) {
    return;
  }

  saveProgress();
  lastSavedPage = static_cast<int32_t>(currentPage);
  progressDirty = false;
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  const std::string progPath = xtc->getCachePath() + "/progress.bin";
  const std::string bakPath = xtc->getCachePath() + "/progress.bin.bak";
  bool opened = Storage.openFileForRead("XTR", progPath, f);
  if (!opened && Storage.exists(bakPath.c_str())) {
    LOG_INF("XTR", "progress.bin missing, recovering from progress.bin.bak");
    opened = Storage.openFileForRead("XTR", bakPath, f);
  }
  if (!opened) {
    const std::string flatName = backup::flatNameForCacheFile(xtc->getCachePath(), "progress.bin");
    if (backup::restoreFromMirror(flatName, progPath)) {
      LOG_INF("XTR", "progress.bin recovered from mirror %s", flatName.c_str());
      opened = Storage.openFileForRead("XTR", progPath, f);
    }
  }
  if (opened) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);
      lastSavedPage = static_cast<int32_t>(currentPage);
      lastObservedPage = static_cast<int32_t>(currentPage);
      progressDirty = false;

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = (xtc->getPageCount() > 0) ? (xtc->getPageCount() - 1) : 0;
      }
    }
    f.close();
  }
}
