#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ReaderLayoutSafety.h"
#include "ReadingThemeStore.h"
#include "ReadingThemesActivity.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "activities/network/QRShareActivity.h"
#include "util/FavoriteBmp.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DrawUtils.h"
#include "util/StatusPopup.h"
#include "util/TransitionFeedback.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long goHomeMs = 1000;
constexpr unsigned long confirmDoubleTapMs = 350;
constexpr unsigned long progressSaveDebounceMs = 800;
int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

using ReaderStatusBar::computeStatusBarsHeight;
using ReaderStatusBar::computeStatusBarReservedHeight;
using ReaderStatusBar::computeStatusTextBlockHeight;
using ReaderStatusBar::getStatusBottomInset;
using ReaderStatusBar::getStatusTopInset;
using ReaderStatusBar::normalizeReaderMargins;
using ReaderStatusBar::statusBarItemIsTop;
using ReaderStatusBar::statusTextPositionIsTop;
using ReaderStatusBar::wrapStatusText;

std::string formatPageCounterText(const uint8_t mode, const int currentPage, const int chapterPageCount,
                                  const float bookProgressPercent) {
  (void)bookProgressPercent;

  const int safeChapterPageCount = std::max(chapterPageCount, 0);
  const int safeCurrentPage = std::max(currentPage, 0);
  int pagesLeft = safeChapterPageCount - (currentPage + 1);
  if (pagesLeft < 0) {
    pagesLeft = 0;
  }

  switch (mode) {
    case CrossPointSettings::STATUS_PAGE_LEFT_TEXT:
      return std::to_string(pagesLeft) + " left";
    default:
      return std::to_string(safeCurrentPage + 1) + "/" + std::to_string(safeChapterPageCount);
  }
}


int resolveCurrentTocIndex(const std::shared_ptr<Epub>& epub, const Section* section, const int currentSpineIndex) {
  if (!epub) {
    return -1;
  }

  if (section != nullptr) {
    int bestTocIndex = -1;
    int bestPage = -1;
    for (const int tocIndex : epub->getTocIndexesForSpineIndex(currentSpineIndex)) {
      const auto tocItem = epub->getTocItem(tocIndex);
      if (tocItem.spineIndex != currentSpineIndex || tocItem.anchor.empty()) {
        continue;
      }

      const int tocPage = section->getPageForAnchor(tocItem.anchor);
      if (tocPage >= 0 && tocPage <= section->currentPage && tocPage >= bestPage) {
        bestPage = tocPage;
        bestTocIndex = tocIndex;
      }
    }

    if (bestTocIndex >= 0) {
      return bestTocIndex;
    }
  }

  return epub->getTocIndexForSpineIndex(currentSpineIndex);
}

// Apply the logical reader orientation to the renderer.
// This centralizes orientation mapping so we don't duplicate switch logic
// elsewhere.
void applyReaderOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  applyReaderOrientation(renderer, SETTINGS.orientation);
  EpdFontFamily::setReaderBoldSwapEnabled(SETTINGS.readerBoldSwap != 0);

  epub->setupCacheDir();

  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = nextPageNumber;
      lastObservedSpineIndex = currentSpineIndex;
      lastObservedPage = nextPageNumber;
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      lastSavedPageCount = cachedChapterTotalPageCount;
      lastObservedPageCount = cachedChapterTotalPageCount;
    }
    f.close();
  }
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount > 0 && currentSpineIndex >= spineCount) {
    currentSpineIndex = spineCount - 1;
    nextPageNumber = UINT16_MAX;
  }

  // We may want a better condition to detect if we are opening for the first
  // time. This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  // Generate cover thumbnail for home screen cover layouts
  epub->generateThumbBmp(400);

  // Move book to /recents/ folder on first open from another location
  {
    std::string newPath = RECENT_BOOKS.moveBookToRecents(epub->getPath());
    if (!newPath.empty()) {
      epub->setPath(newPath);
    }
  }

  invalidateStatusBarCaches();
  clearPageCache();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  flushProgressIfNeeded(true);
  pendingMenuOpen = false;
  highlightState = HighlightState::NONE;
  EpdFontFamily::setReaderBoldSwapEnabled(false);
  ActivityWithSubactivity::onExit();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  clearPageCache();
  section.reset();
  epub.reset();
  invalidateStatusBarCaches();
}

void EpubReaderActivity::invalidateStatusBarCaches() {
  cachedReserveSpineIndex = -1;
  cachedReserveUsableWidth = -1;
  cachedReserveNoTitleTruncation = false;
  cachedReserveTitleLineCount = 1;
  cachedTitleTocIndex = -2;
  cachedTitleUsableWidth = -1;
  cachedTitleNoTitleTruncation = false;
  cachedTitleMaxLines = -1;
  cachedTitleLines.clear();
}

void EpubReaderActivity::clearPageCache() {
  for (auto& entry : pageCache) {
    entry.pageIndex = -1;
    entry.page.reset();
  }
  pageCacheSpineIndex = -1;
}

std::shared_ptr<Page> EpubReaderActivity::getCachedPage(const int pageIndex) const {
  if (pageCacheSpineIndex != currentSpineIndex) {
    return {};
  }

  const auto it = std::find_if(pageCache.begin(), pageCache.end(),
                               [pageIndex](const PageCacheEntry& entry) { return entry.pageIndex == pageIndex; });
  if (it != pageCache.end()) {
    return it->page;
  }

  return {};
}

std::shared_ptr<Page> EpubReaderActivity::loadAndCachePage(const int pageIndex) {
  if (!section) {
    return {};
  }

  auto page = std::shared_ptr<Page>(section->loadPageFromSectionFile(pageIndex));
  if (!page) {
    return {};
  }

  pageCacheSpineIndex = currentSpineIndex;
  const auto it = std::find_if(pageCache.begin(), pageCache.end(),
                               [pageIndex](const PageCacheEntry& entry) { return entry.pageIndex == pageIndex; });
  if (it != pageCache.end()) {
    it->page = page;
    return page;
  }

  pageCache[0] = pageCache[1];
  pageCache[1] = pageCache[2];
  pageCache[2] = PageCacheEntry{.pageIndex = pageIndex, .page = std::move(page)};
  return pageCache[2].page;
}

void EpubReaderActivity::refreshPageCacheWindow(const int centerPage, const std::shared_ptr<Page>& currentPage) {
  if (!section || centerPage < 0 || centerPage >= section->pageCount) {
    clearPageCache();
    return;
  }

  std::array<PageCacheEntry, 3> nextWindow{};
  const int targets[3] = {centerPage - 1, centerPage, centerPage + 1};

  for (size_t i = 0; i < nextWindow.size(); i++) {
    const int targetPage = targets[i];
    if (targetPage < 0 || targetPage >= section->pageCount) {
      continue;
    }

    std::shared_ptr<Page> page;
    if (targetPage == centerPage) {
      page = currentPage;
    } else {
      page = getCachedPage(targetPage);
      if (!page) {
        page = std::shared_ptr<Page>(section->loadPageFromSectionFile(targetPage));
      }
    }

    nextWindow[i] = PageCacheEntry{.pageIndex = targetPage, .page = std::move(page)};
  }

  pageCache = std::move(nextWindow);
  pageCacheSpineIndex = currentSpineIndex;
}

int EpubReaderActivity::getWrappedStatusBarReserveLineCount(const int usableWidth) {
  if (!epub || usableWidth <= 0) {
    return 1;
  }
  if (cachedReserveSpineIndex == currentSpineIndex && cachedReserveUsableWidth == usableWidth &&
      cachedReserveNoTitleTruncation == SETTINGS.statusBarNoTitleTruncation) {
    return cachedReserveTitleLineCount;
  }

  int maxLines = 1;
  auto tocIndexes = epub->getTocIndexesForSpineIndex(currentSpineIndex);
  if (tocIndexes.empty()) {
    const int fallbackIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (fallbackIndex >= 0) {
      tocIndexes.push_back(fallbackIndex);
    }
  }

  for (const int tocIndex : tocIndexes) {
    const std::string title = epub->formatTocDisplayTitle(tocIndex);
    if (title.empty()) {
      continue;
    }
    const int lineCount = static_cast<int>(wrapStatusText(renderer, SETTINGS.getStatusBarFontId(), title, usableWidth).size());
    if (lineCount > maxLines) {
      maxLines = lineCount;
    }
  }

  cachedReserveSpineIndex = currentSpineIndex;
  cachedReserveUsableWidth = usableWidth;
  cachedReserveNoTitleTruncation = SETTINGS.statusBarNoTitleTruncation;
  cachedReserveTitleLineCount = maxLines;
  return cachedReserveTitleLineCount;
}

const std::vector<std::string>& EpubReaderActivity::getStatusBarTitleLines(const int tocIndex, const int usableWidth,
                                                                           const bool noTitleTruncation,
                                                                           const int maxTitleLineCount) {
  if (cachedTitleTocIndex == tocIndex && cachedTitleUsableWidth == usableWidth &&
      cachedTitleNoTitleTruncation == noTitleTruncation && cachedTitleMaxLines == maxTitleLineCount) {
    return cachedTitleLines;
  }

  std::string titleText = tr(STR_UNNAMED);
  if (tocIndex >= 0 && epub) {
    titleText = epub->formatTocDisplayTitle(tocIndex);
    if (titleText.empty()) {
      titleText = tr(STR_UNNAMED);
    }
  }

  cachedTitleLines = ReaderLayoutSafety::buildTitleLines(renderer, SETTINGS.getStatusBarFontId(), titleText, usableWidth,
                                                         noTitleTruncation, maxTitleLineCount);

  cachedTitleTocIndex = tocIndex;
  cachedTitleUsableWidth = usableWidth;
  cachedTitleNoTitleTruncation = noTitleTruncation;
  cachedTitleMaxLines = maxTitleLineCount;
  return cachedTitleLines;
}

EpubReaderActivity::StatusBarLayout EpubReaderActivity::buildStatusBarLayout(const int usableWidth,
                                                                             const int topReservedHeight,
                                                                             const int bottomReservedHeight,
                                                                             const int maxTitleLineCount) {
  StatusBarLayout layout;
  layout.usableWidth = ReaderLayoutSafety::clampViewportDimension(usableWidth, ReaderLayoutSafety::kMinViewportWidth,
                                                                  "ERS", "status width");
  layout.topReservedHeight = topReservedHeight;
  layout.bottomReservedHeight = bottomReservedHeight;
  if (!SETTINGS.statusBarEnabled || !section) {
    return layout;
  }

  const float sectionChapterProg =
      (section->pageCount > 0) ? static_cast<float>(section->currentPage) / section->pageCount : 0.0f;
  layout.bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100.0f;
  layout.chapterProgress =
      (section->pageCount > 0) ? (static_cast<float>(section->currentPage + 1) / section->pageCount) * 100.0f : 0.0f;

  if (SETTINGS.statusBarShowPageCounter) {
    layout.pageCounterText = formatPageCounterText(SETTINGS.statusBarPageCounterMode, section->currentPage,
                                                   section->pageCount, layout.bookProgress);
    layout.pageCounterTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.pageCounterText.c_str());
  }
  if (SETTINGS.statusBarShowBookPercentage) {
    char bookPercentageStr[16] = {0};
    snprintf(bookPercentageStr, sizeof(bookPercentageStr), "B:%.0f%%", layout.bookProgress);
    layout.bookPercentageText = bookPercentageStr;
    layout.bookPercentageTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.bookPercentageText.c_str());
  }
  if (SETTINGS.statusBarShowChapterPercentage) {
    char chapterPercentageStr[16] = {0};
    snprintf(chapterPercentageStr, sizeof(chapterPercentageStr), "C:%.0f%%", layout.chapterProgress);
    layout.chapterPercentageText = chapterPercentageStr;
    layout.chapterPercentageTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.chapterPercentageText.c_str());
  }

  if (SETTINGS.statusBarShowChapterTitle) {
    constexpr int titlePadding = 4;
    const int titleWrapWidth = renderer.getScreenWidth() - titlePadding * 2;
    const int tocIndex = section->getTocIndexForPage(section->currentPage);
    layout.titleLines =
        getStatusBarTitleLines(tocIndex, titleWrapWidth, SETTINGS.statusBarNoTitleTruncation, maxTitleLineCount);
    layout.titleLineWidths.reserve(layout.titleLines.size());
    for (const auto& line : layout.titleLines) {
      layout.titleLineWidths.push_back(renderer.getTextWidth(SETTINGS.getStatusBarFontId(), line.c_str()));
    }
  }

  return layout;
}

void EpubReaderActivity::loop() {
  flushProgressIfNeeded(false);

  if (subActivity) { loopSubActivity(); return; }

  // Handle pending navigation callbacks (deferred to avoid use-after-free)
  if (pendingGoHome) {
    pendingGoHome = false;
    if (onGoHome) onGoHome();
    return;
  }
  if (pendingGoLibrary) {
    pendingGoLibrary = false;
    if (onGoBack) onGoBack();
    return;
  }

  // Highlight mode intercepts all input while active
  if (highlightState != HighlightState::NONE) { loopHighlightMode(); return; }

  if (pendingMenuOpen && !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      millis() - lastConfirmReleaseMs > confirmDoubleTapMs) {
    pendingMenuOpen = false;
    openReaderMenu();
    return;
  }

  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    confirmLongPressHandled = false;
  }

  // Single tap opens menu; double tap toggles text render mode (Dark/Crisp).
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

  // Long press CONFIRM (1s+) enters text highlight/quote selection mode.
  if (!confirmLongPressHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= goHomeMs) {
    confirmLongPressHandled = true;
    mappedInput.suppressUntilAllReleased();
    enterHighlightMode();
    return;
  }

  // BACK: go home immediately on press for snappier response.
  // If viewing a footnote, restore saved position instead.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // Determine page turn triggers
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

  if (prevTriggered || nextTriggered) {
    loopPageTurn(prevTriggered, nextTriggered);
  }
}

void EpubReaderActivity::loopSubActivity() {
  subActivity->loop();
  // Deferred exit: process after subActivity->loop() returns to avoid use-after-free
  if (pendingSubactivityExit) {
    pendingSubactivityExit = false;
    const bool shouldReloadTheme = pendingThemeReload;
    pendingThemeReload = false;
    exitActivity();
    if (shouldReloadTheme) {
      reloadCurrentSectionForDisplaySettings();
    } else {
      requestUpdate();
    }
  }
  // Deferred go home: process after subActivity->loop() returns to avoid race condition
  if (pendingGoHome) {
    pendingGoHome = false;
    exitActivity();
    if (onGoHome) onGoHome();
  }
}

void EpubReaderActivity::loopHighlightMode() {
  // SHOW_UNDERLINE: wait 3 seconds then save quote and exit
  if (highlightState == HighlightState::SHOW_UNDERLINE) {
    if (millis() - highlightUnderlineStartMs >= 3000) {
      std::string quote = extractQuoteText();
      if (!quote.empty()) {
        saveQuoteToFile(quote);
        StatusPopup::showBlocking(renderer, "Quote saved!");
      }
      exitHighlightMode();
    }
    return;
  }
  handleHighlightInput();
}

void EpubReaderActivity::loopPageTurn(bool prevTriggered, bool nextTriggered) {
  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
    }
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  // Don't skip chapter after screenshot
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) && mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  if (skipChapter) {
    TransitionFeedback::show(renderer, tr(STR_LOADING));
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      saveProgress(currentSpineIndex, nextPageNumber, 1);
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = nextPageNumber;
      lastSavedPageCount = 1;
      progressDirty = false;
      clearPageCache();
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
      progressDirty = true;
      lastProgressChangeMs = millis();
      flushProgressIfNeeded(true);
    } else if (currentSpineIndex > 0) {
      TransitionFeedback::show(renderer, tr(STR_LOADING));
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        saveProgress(currentSpineIndex, nextPageNumber, 1);
        lastSavedSpineIndex = currentSpineIndex;
        lastSavedPage = nextPageNumber;
        lastSavedPageCount = 1;
        progressDirty = false;
        clearPageCache();
        section.reset();
      }
    }
    requestUpdate();
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
      addSessionPagesRead();
      progressDirty = true;
      lastProgressChangeMs = millis();
      flushProgressIfNeeded(true);
    } else {
      TransitionFeedback::show(renderer, tr(STR_LOADING));
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        const bool hasNextSection = epub && currentSpineIndex + 1 < epub->getSpineItemsCount();
        currentSpineIndex++;
        if (hasNextSection) {
          addSessionPagesRead();
        }
        saveProgress(currentSpineIndex, nextPageNumber, 1);
        lastSavedSpineIndex = currentSpineIndex;
        lastSavedPage = nextPageNumber;
        lastSavedPageCount = 1;
        progressDirty = false;
        clearPageCache();
        section.reset();
      }
    }
    requestUpdate();
  }
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;
  float bookProgress = 0.0f;
  if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  exitActivity();
  enterNewActivity(new EpubReaderMenuActivity(
      this->renderer, this->mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
      SETTINGS.orientation, !currentPageFootnotes.empty(),
      [this](const uint8_t orientation) { onReaderMenuBack(orientation); },
      [this](EpubReaderMenuActivity::MenuAction action) { onReaderMenuConfirm(action); }));
}

void EpubReaderActivity::toggleTextRenderMode() {
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  flushProgressIfNeeded(true);
  SETTINGS.textRenderMode = (SETTINGS.textRenderMode + 1) % CrossPointSettings::TEXT_RENDER_MODE_COUNT;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("ERS", "Failed to save settings after text render mode toggle");
  }

  uint16_t backupSpine = 0;
  uint16_t backupPage = 0;
  uint16_t backupPageCount = 1;
  if (epub) {
    const uint16_t spineCount = epub->getSpineItemsCount();
    if (section && section->pageCount > 0) {
      backupSpine = currentSpineIndex;
      backupPage = section->currentPage;
      backupPageCount = section->pageCount;
    } else if (spineCount > 0) {
      if (currentSpineIndex >= spineCount) {
        backupSpine = spineCount - 1;
        backupPage = UINT16_MAX;
      } else {
        backupSpine = currentSpineIndex;
      }
    }
  }

  {
    RenderLock lock(*this);
    clearPageCache();
    section.reset();
    saveProgress(backupSpine, backupPage, backupPageCount);
    lastSavedSpineIndex = backupSpine;
    lastSavedPage = backupPage;
    lastSavedPageCount = backupPageCount;
    lastObservedSpineIndex = backupSpine;
    lastObservedPage = backupPage;
    lastObservedPageCount = backupPageCount;
    progressDirty = false;
    nextPageNumber = backupPage;
    cachedSpineIndex = backupSpine;
    cachedChapterTotalPageCount = backupPageCount;
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuBack(const uint8_t orientation) {
  exitActivity();
  // Apply the user-selected orientation when the menu is dismissed.
  // This ensures the menu can be navigated without immediately rotating the
  // screen.
  applyOrientation(orientation);
  requestUpdate();
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize %
  // 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once
  // loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    clearPageCache();
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      // Calculate values BEFORE we start destroying things
      const int spineIdx = currentSpineIndex;
      const int currentTocIndex = section ? section->getTocIndexForPage(section->currentPage)
                                          : resolveCurrentTocIndex(epub, section.get(), currentSpineIndex);
      const std::string path = epub->getPath();

      // 1. Close the menu
      exitActivity();

      // 2. Open the Chapter Selector
      enterNewActivity(new EpubReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, epub, path, spineIdx, currentTocIndex,
          [this] {
            exitActivity();
            requestUpdate();
          },
          [this](const int tocIndex) {
            const auto tocItem = epub->getTocItem(tocIndex);
            const int newSpineIndex = tocItem.spineIndex;
            if (newSpineIndex < 0) {
              exitActivity();
              requestUpdate();
              return;
            }

            pendingAnchor = tocItem.anchor;
            if (currentSpineIndex != newSpineIndex || section) {
              TransitionFeedback::show(renderer, tr(STR_LOADING));
              currentSpineIndex = newSpineIndex;
              nextPageNumber = 0;
              clearPageCache();
              section.reset();
            }
            exitActivity();
            requestUpdate();
          },
          [this](const int newSpineIndex, const int newPage) {
            if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
              TransitionFeedback::show(renderer, tr(STR_LOADING));
              currentSpineIndex = newSpineIndex;
              nextPageNumber = newPage;
              clearPageCache();
              section.reset();
            }
            exitActivity();
            requestUpdate();
          }));

      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      exitActivity();
      enterNewActivity(new EpubReaderFootnotesActivity(
          this->renderer, this->mappedInput, currentPageFootnotes,
          [this] {
            // Go back from footnotes list
            exitActivity();
            requestUpdate();
          },
          [this](const char* href) {
            // Navigate to selected footnote
            navigateToHref(href, true);
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    // GO_HOME menu item removed — Back button handles this
    case EpubReaderMenuActivity::MenuAction::THEMES_MENU: {
      exitActivity();
      enterNewActivity(new ReadingThemesActivity(renderer, mappedInput, epub ? epub->getCachePath() : std::string(),
                                                 [this](const bool changed) {
                                                   pendingSubactivityExit = true;
                                                   pendingMenuOpen = false;
                                                   pendingThemeReload = changed;
                                                 }));
      break;
    }
    // REVERT_THEME menu item removed — use Reading Themes to manage themes
    case EpubReaderMenuActivity::MenuAction::TRIAGE_FAVORITE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      std::string updatedPath;
      const bool makeFavorite = !FavoriteBmp::isFavoritePath(lastPath);
      const auto result = FavoriteBmp::setFavorite(lastPath, makeFavorite, &updatedPath);
      if (result == FavoriteBmp::SetFavoriteResult::LimitReached) {
        StatusPopup::showBlocking(renderer, FavoriteBmp::limitReachedPopupMessage());
      } else if (result == FavoriteBmp::SetFavoriteResult::RenameConflict) {
        StatusPopup::showBlocking(renderer, "Favorite name already exists");
      } else if (result != FavoriteBmp::SetFavoriteResult::Success) {
        StatusPopup::showBlocking(renderer, "Favorite failed");
      } else {
        StatusPopup::showBlocking(renderer, makeFavorite ? "Favorited" : "Unfavorited");
      }
      delay(500);
      exitActivity();
      pendingMenuOpen = false;
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_PAUSE_ROTATION: {
      APP_STATE.wallpaperRotationPaused = !APP_STATE.wallpaperRotationPaused;
      APP_STATE.saveToFile();
      StatusPopup::showBlocking(renderer,
                                APP_STATE.wallpaperRotationPaused ? "Rotation paused"
                                                                 : "Rotation unpaused");
      delay(500);
      exitActivity();
      pendingMenuOpen = false;
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_MOVE_PAUSE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      if (lastPath.rfind("/sleep pause/", 0) == 0) {
        StatusPopup::showBlocking(renderer, "Already in sleep pause");
        delay(500);
        exitActivity();
        pendingMenuOpen = false;
        // Input suppression handled by exitActivity()
        requestUpdate();
        break;
      }
      const std::string destDir = "/sleep pause";
      Storage.mkdir(destDir.c_str());
      const auto slashPos = lastPath.find_last_of('/');
      const std::string filename =
          (slashPos == std::string::npos) ? lastPath : lastPath.substr(slashPos + 1);
      const std::string dstPath = destDir + "/" + filename;
      FsFile src, dst;
      bool ok = false;
      if (Storage.openFileForRead("TRG", lastPath.c_str(), src) &&
          Storage.openFileForWrite("TRG", dstPath.c_str(), dst)) {
        uint8_t buf[512];
        ok = true;
        while (src.available()) {
          const int n = src.read(buf, sizeof(buf));
          if (n <= 0 || dst.write(buf, n) != n) { ok = false; break; }
        }
        src.close();
        dst.close();
        if (ok) {
          Storage.remove(lastPath.c_str());
          FavoriteBmp::replacePathReferences(lastPath, dstPath);
          APP_STATE.wallpaperRotationPaused = false;
          APP_STATE.saveToFile();
        } else {
          Storage.remove(dstPath.c_str());
        }
      } else {
        if (src) src.close();
        if (dst) dst.close();
      }
      StatusPopup::showBlocking(renderer, ok ? "Moved to sleep pause" : "Move failed");
      delay(500);
      exitActivity();
      pendingMenuOpen = false;
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_DELETE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      const bool removed = Storage.remove(lastPath.c_str());
      if (removed) {
        FavoriteBmp::removePathReferences(lastPath);
        APP_STATE.wallpaperRotationPaused = false;
        APP_STATE.saveToFile();
      }
      StatusPopup::showBlocking(renderer, removed ? "Wallpaper deleted" : "Delete failed");
      delay(500);
      exitActivity();
      pendingMenuOpen = false;
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      exitActivity();
      enterNewActivity(new ConfirmDialogActivity(
          renderer, mappedInput, "Clear cached pages and reset reading progress to page 1?",
          [this]() {
            // Confirmed — clear cache and reset progress.
            exitActivity();
            StatusPopup::showBlocking(renderer, "Clearing book cache");
            {
              RenderLock lock(*this);
              if (epub) {
                const uint16_t resetSpine = 0;
                const uint16_t resetPage = 0;
                const uint16_t resetPageCount = 1;

                section.reset();
                clearPageCache();
                epub->clearCache();
                epub->setupCacheDir();
                saveProgress(resetSpine, resetPage, resetPageCount);

                currentSpineIndex = resetSpine;
                nextPageNumber = resetPage;
                cachedSpineIndex = resetSpine;
                cachedChapterTotalPageCount = resetPageCount;
                lastSavedSpineIndex = resetSpine;
                lastSavedPage = resetPage;
                lastSavedPageCount = resetPageCount;
                lastObservedSpineIndex = resetSpine;
                lastObservedPage = resetPage;
                lastObservedPageCount = resetPageCount;
                progressDirty = false;
              }
            }
            pendingGoHome = true;
          },
          [this]() {
            // Cancelled — return to reader.
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : 0;
        const int totalPages = section ? section->pageCount : 0;
        exitActivity();
        enterNewActivity(new KOReaderSyncActivity(
            renderer, mappedInput, epub, epub->getPath(), currentSpineIndex, currentPage, totalPages,
            [this]() {
              // On cancel - defer exit to avoid use-after-free
              pendingSubactivityExit = true;
            },
            [this](int newSpineIndex, int newPage) {
              // On sync complete - update position and defer exit
              if (currentSpineIndex != newSpineIndex || (section && section->currentPage != newPage)) {
                TransitionFeedback::show(renderer, tr(STR_LOADING));
                currentSpineIndex = newSpineIndex;
                nextPageNumber = newPage;
                clearPageCache();
                section.reset();
              }
              pendingSubactivityExit = true;
            }));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOK: {
      std::string deletingPath;
      StatusPopup::showBlocking(renderer, "Deleting book");
      {
        RenderLock lock(*this);
        if (epub) {
          deletingPath = epub->getPath();
          clearPageCache();
          section.reset();
          epub->clearCache();
        }
      }

      if (!deletingPath.empty()) {
        RECENT_BOOKS.removeBook(deletingPath);
        if (APP_STATE.openEpubPath == deletingPath) {
          APP_STATE.openEpubPath = "";
          APP_STATE.saveToFile();
        }
        const bool removed = Storage.remove(deletingPath.c_str());
        LOG_DBG("ERS", "Delete book '%s': %s", deletingPath.c_str(), removed ? "ok" : "failed");
      }
      pendingGoLibrary = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::REMOVE_FROM_RECENT: {
      if (epub) {
        RECENT_BOOKS.removeBook(epub->getPath());
        if (APP_STATE.openEpubPath == epub->getPath()) {
          APP_STATE.openEpubPath = "";
          APP_STATE.saveToFile();
        }
      }
      pendingGoHome = true;
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SHARE_QR: {
      if (epub) {
        exitActivity();
        enterNewActivity(new QRShareActivity(renderer, mappedInput,
            [this] { exitActivity(); requestUpdate(); }, epub->getPath()));
      }
      break;
    }
    default:
      break;
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  TransitionFeedback::show(renderer, tr(STR_LOADING));

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next
    // launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    applyReaderOrientation(renderer, SETTINGS.orientation);

    invalidateStatusBarCaches();

    // Reset section to force re-layout in the new orientation.
    clearPageCache();
    section.reset();
  }
}

void EpubReaderActivity::reloadCurrentSectionForDisplaySettings() {
  TransitionFeedback::show(renderer, tr(STR_LOADING));
  flushProgressIfNeeded(true);
  if (epub && SETTINGS.readerStyleMode == CrossPointSettings::READER_STYLE_HYBRID) {
    const bool showCssProgress = epub->getCssParser() == nullptr || !epub->getCssParser()->hasCache();
    const auto progressCallback = std::function<void(int)>();
    if (!epub->ensureCssCache(progressCallback)) {
      LOG_ERR("ERS", "Failed to prepare CSS cache for hybrid reader style");
    } else if (showCssProgress) {
      TransitionFeedback::dismiss(renderer);
    }
  }
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
      saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->pageCount;
      lastObservedSpineIndex = currentSpineIndex;
      lastObservedPage = section->currentPage;
      lastObservedPageCount = section->pageCount;
      progressDirty = false;
    }
    invalidateStatusBarCaches();
    clearPageCache();
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::render(Activity::RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::REGULAR);
    renderer.displayBuffer();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  normalizeReaderMargins(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom, &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMarginTop;
  orientedMarginLeft += SETTINGS.screenMarginHorizontal;
  orientedMarginRight += SETTINGS.screenMarginHorizontal;
  orientedMarginBottom += SETTINGS.screenMarginBottom;
  const int minContentHeight =
      std::max(ReaderLayoutSafety::kMinViewportHeight, renderer.getLineHeight(SETTINGS.getReaderFontId()) * 2);

  const int usableWidth =
      ReaderLayoutSafety::clampViewportDimension(renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
                                                 ReaderLayoutSafety::kMinViewportWidth, "ERS", "usable width");
  int statusBarTopReserved = 0;
  int statusBarBottomReserved = 0;
  int resolvedTitleLineCount = SETTINGS.statusBarShowChapterTitle ? 1 : 0;
  if (SETTINGS.statusBarEnabled) {
    const bool showTopStatusTextRow =
        (SETTINGS.statusBarShowBattery && statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter && statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage && statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition));
    const bool showBottomStatusTextRow =
        (SETTINGS.statusBarShowBattery && !statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter && !statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage && !statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         !statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition));
    int titleLineCount = SETTINGS.statusBarShowChapterTitle ? 1 : 0;
    if (SETTINGS.statusBarShowChapterTitle && SETTINGS.statusBarNoTitleTruncation) {
      constexpr int titlePadding = 4;
      const int titleWrapWidth = renderer.getScreenWidth() - titlePadding * 2;
      titleLineCount = getWrappedStatusBarReserveLineCount(titleWrapWidth);
    }
    const int topTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle && statusBarItemIsTop(SETTINGS.statusBarTitlePosition)) ? titleLineCount
                                                                                                    : 0;
    const int bottomTitleLineCount =
        (SETTINGS.statusBarShowChapterTitle && !statusBarItemIsTop(SETTINGS.statusBarTitlePosition)) ? titleLineCount
                                                                                                     : 0;
    const auto budget = ReaderLayoutSafety::resolveStatusBarBudget(
        renderer, SETTINGS.getStatusBarFontId(), "ERS", renderer.getScreenHeight(), getStatusTopInset(renderer), getStatusBottomInset(renderer),
        SETTINGS.screenMarginTop, SETTINGS.screenMarginBottom, minContentHeight,
        SETTINGS.getStatusBarProgressBarHeight(),
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showTopStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar && statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar && statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = topTitleLineCount,
        },
        ReaderLayoutSafety::StatusBarBandConfig{
            .showStatusTextRow = showBottomStatusTextRow,
            .showBookProgressBar =
                SETTINGS.statusBarShowBookBar && !statusBarItemIsTop(SETTINGS.statusBarBookBarPosition),
            .showChapterProgressBar =
                SETTINGS.statusBarShowChapterBar && !statusBarItemIsTop(SETTINGS.statusBarChapterBarPosition),
            .desiredTitleLineCount = bottomTitleLineCount,
        });
    statusBarTopReserved = budget.top.reservedHeight;
    statusBarBottomReserved = budget.bottom.reservedHeight;
    resolvedTitleLineCount =
        statusBarItemIsTop(SETTINGS.statusBarTitlePosition) ? budget.top.titleLineCount : budget.bottom.titleLineCount;
    if (statusBarTopReserved > 0) {
      orientedMarginTop = getStatusTopInset(renderer) + SETTINGS.screenMarginTop + statusBarTopReserved;
    }
    if (statusBarBottomReserved > 0) {
      // When the status bar is present it handles the display bottom inset
      // itself. Use only the display inset + user margin so the gap equals
      // exactly screenMarginBottom (0 = text flush against the status bar).
      orientedMarginBottom = getStatusBottomInset(renderer) + SETTINGS.screenMarginBottom + statusBarBottomReserved;
    }
  }

  const uint16_t viewportWidth = static_cast<uint16_t>(
      ReaderLayoutSafety::clampViewportDimension(renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight,
                                                 ReaderLayoutSafety::kMinViewportWidth, "ERS", "viewport width"));
  const uint16_t viewportHeight = static_cast<uint16_t>(
      ReaderLayoutSafety::clampViewportDimension(renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom,
                                                 minContentHeight, "ERS", "viewport height"));

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    bool builtSection = false;
    clearPageCache();

    const uint8_t sectionTextRenderMode = SETTINGS.textRenderMode;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent, SETTINGS.firstLineIndentMode,
                                  SETTINGS.readerStyleMode, sectionTextRenderMode, SETTINGS.readerBoldSwap != 0)) {
      LOG_DBG("ERS", "Cache not found, building...");
      builtSection = true;
      TransitionFeedback::show(renderer, tr(STR_LOADING));

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent, SETTINGS.firstLineIndentMode,
                                      SETTINGS.readerStyleMode, sectionTextRenderMode, SETTINGS.readerBoldSwap != 0,
                                      nullptr)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        clearPageCache();
        section.reset();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    // handles changes in reader settings and reset to approximate position
    // based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page
      // count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }

    if (!pendingAnchor.empty()) {
      const int anchorPage = section->getPageForAnchor(pendingAnchor);
      if (anchorPage >= 0 && anchorPage < section->pageCount) {
        section->currentPage = anchorPage;
      }
      pendingAnchor.clear();
    }

    TransitionFeedback::dismiss(renderer);
  }

  renderer.clearScreen();
  const StatusBarLayout statusBarLayout =
      buildStatusBarLayout(usableWidth, statusBarTopReserved, statusBarBottomReserved, resolvedTitleLineCount);

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::REGULAR);
    renderStatusBar(statusBarLayout, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    renderer.displayBuffer();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d), clamping", section->currentPage, section->pageCount);
    if (section->currentPage < 0) {
      section->currentPage = 0;
    } else {
      section->currentPage = section->pageCount - 1;
    }
  }

  {
    auto p = getCachedPage(section->currentPage);
    if (!p) {
      p = loadAndCachePage(section->currentPage);
    }
    if (!p) {
      pageLoadFailCount++;
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache (attempt %d)", pageLoadFailCount);
      section->clearCache();
      clearPageCache();
      section.reset();
      if (pageLoadFailCount < 3) {
        requestUpdate();  // Try again after clearing cache
      } else {
        LOG_ERR("ERS", "Page load failed %d times, giving up to prevent infinite loop", pageLoadFailCount);
      }
      return;
    }
    pageLoadFailCount = 0;
    refreshPageCacheWindow(section->currentPage, p);

    // Collect footnotes from the loaded page (copy, not move, to preserve cached page data)
    currentPageFootnotes = p->footnotes;
    const auto start = millis();
    renderContents(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft,
                   statusBarLayout);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  if (lastObservedSpineIndex != currentSpineIndex || lastObservedPage != section->currentPage ||
      lastObservedPageCount != section->pageCount) {
    lastObservedSpineIndex = currentSpineIndex;
    lastObservedPage = section->currentPage;
    lastObservedPageCount = section->pageCount;
    if (lastSavedSpineIndex != currentSpineIndex || lastSavedPage != section->currentPage ||
        lastSavedPageCount != section->pageCount) {
      progressDirty = true;
      lastProgressChangeMs = millis();
    }
  }

  flushProgressIfNeeded(false);
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  const uint8_t sectionTextRenderMode = SETTINGS.textRenderMode;

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent, SETTINGS.firstLineIndentMode,
                                  SETTINGS.readerStyleMode, sectionTextRenderMode, SETTINGS.readerBoldSwap != 0)) {
    return;  // Already cached
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent, SETTINGS.firstLineIndentMode,
                                     SETTINGS.readerStyleMode, sectionTextRenderMode, SETTINGS.readerBoldSwap != 0)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  if (!epub) {
    return;
  }
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    return;
  }
  if (spineIndex < 0) {
    spineIndex = 0;
  } else if (spineIndex >= spineCount) {
    spineIndex = spineCount - 1;
    currentPage = UINT16_MAX;
  }
  if (pageCount <= 0) {
    pageCount = 1;
  }

  const std::string progPath = epub->getCachePath() + "/progress.bin";
  const std::string tmpPath = epub->getCachePath() + "/progress_tmp.bin";

  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  FsFile f;
  if (Storage.openFileForWrite("ERS", tmpPath.c_str(), f)) {
    uint8_t data[6];
    data[0] = spineIndex & 0xFF;
    data[1] = (spineIndex >> 8) & 0xFF;
    data[2] = currentPage & 0xFF;
    data[3] = (currentPage >> 8) & 0xFF;
    data[4] = pageCount & 0xFF;
    data[5] = (pageCount >> 8) & 0xFF;
    f.write(data, 6);
    f.close();

    if (Storage.exists(progPath.c_str())) {
      Storage.remove(progPath.c_str());
    }
    Storage.rename(tmpPath.c_str(), progPath.c_str());

    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d", spineIndex, currentPage);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
void EpubReaderActivity::flushProgressIfNeeded(const bool force) {
  if (!epub || !section || section->pageCount == 0) {
    return;
  }
  if (!progressDirty) {
    return;
  }

  const auto now = millis();
  if (!force && now - lastProgressChangeMs < progressSaveDebounceMs) {
    return;
  }

  saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  lastSavedSpineIndex = currentSpineIndex;
  lastSavedPage = section->currentPage;
  lastSavedPageCount = section->pageCount;
  progressDirty = false;
}

void EpubReaderActivity::addSessionPagesRead(const uint32_t amount) { APP_STATE.sessionPagesRead += amount; }

// ── Highlight / Quote selection mode ──────────────────────────────────────────

std::vector<EpubReaderActivity::WordInfo> EpubReaderActivity::buildWordList(const Page& page, const int xOffset,
                                                                            const int yOffset, const int fontId) const {
  std::vector<WordInfo> result;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& tb = line.getTextBlock();
    const auto& words = tb.getWords();
    const auto& xpos = tb.getWordXpos();
    const auto& styles = tb.getWordStyles();
    const int16_t ls = tb.getLetterSpacing();
    for (size_t i = 0; i < words.size(); i++) {
      WordInfo wi;
      wi.x = static_cast<int>(xpos[i]) + line.xPos + xOffset;
      wi.y = line.yPos + yOffset;
      wi.width = renderer.getTextWidthSpaced(fontId, words[i].c_str(), ls, styles[i]);
      wi.text = words[i];
      wi.style = styles[i];
      wi.letterSpacing = ls;
      result.push_back(std::move(wi));
    }
  }
  return result;
}

bool EpubReaderActivity::lookupWordInfo(const Page& page, const int wordIndex, const int xOffset, const int yOffset,
                                        const int fontId, WordInfo& out) const {
  int idx = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    const auto& tb = line.getTextBlock();
    const auto& words = tb.getWords();
    const auto& xpos = tb.getWordXpos();
    const auto& styles = tb.getWordStyles();
    const int16_t ls = tb.getLetterSpacing();
    for (size_t i = 0; i < words.size(); i++) {
      if (idx == wordIndex) {
        out.x = static_cast<int>(xpos[i]) + line.xPos + xOffset;
        out.y = line.yPos + yOffset;
        out.width = renderer.getTextWidthSpaced(fontId, words[i].c_str(), ls, styles[i]);
        out.text = words[i];
        out.style = styles[i];
        out.letterSpacing = ls;
        return true;
      }
      idx++;
    }
  }
  return false;
}

void EpubReaderActivity::rebuildHighlightWordCache(const int xOffset, const int yOffset) {
  highlightWordCache.clear();
  auto page = loadAndCachePage(section->currentPage);
  if (page) {
    const int fontId = SETTINGS.getReaderFontId();
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*el);
      const auto& tb = line.getTextBlock();
      const auto& words = tb.getWords();
      const auto& xpos = tb.getWordXpos();
      const auto& styles = tb.getWordStyles();
      const int16_t ls = tb.getLetterSpacing();
      for (size_t i = 0; i < words.size(); i++) {
        WordPos wp;
        wp.x = static_cast<int16_t>(static_cast<int>(xpos[i]) + line.xPos + xOffset);
        wp.y = static_cast<int16_t>(line.yPos + yOffset);
        wp.width = static_cast<int16_t>(renderer.getTextWidthSpaced(fontId, words[i].c_str(), ls, styles[i]));
        highlightWordCache.push_back(wp);
      }
    }
  }
  highlightWordCachePage = section->currentPage;
}

int EpubReaderActivity::highlightWordCount() const {
  return static_cast<int>(highlightWordCache.size());
}

void EpubReaderActivity::enterHighlightMode() {
  if (!section || section->pageCount == 0) return;
  highlightState = HighlightState::SELECT_START;
  highlightStartSpine = -1;
  highlightStartPage = -1;
  highlightStartWordIndex = -1;
  highlightEndPage = -1;
  highlightEndWordIndex = -1;
  highlightUnderlineStartMs = 0;
  highlightWordCachePage = -1;
  highlightWordCache.clear();

  // Always start cursor at first word
  highlightCursorIndex = 0;
  requestUpdate();
}

void EpubReaderActivity::exitHighlightMode() {
  highlightState = HighlightState::NONE;
  highlightCursorIndex = 0;
  highlightStartSpine = -1;
  highlightStartPage = -1;
  highlightStartWordIndex = -1;
  highlightEndPage = -1;
  highlightEndWordIndex = -1;
  highlightUnderlineStartMs = 0;
  // Free cached word list memory
  highlightWordCache.clear();
  highlightWordCache.shrink_to_fit();
  highlightWordCachePage = -1;
  requestUpdate();
}

void EpubReaderActivity::highlightMoveCursor(const int direction) {
  if (!section) return;
  const int wordCount = highlightWordCount();

  if (highlightState == HighlightState::SELECT_START) {
    if (wordCount == 0) return;

    int newIndex = highlightCursorIndex + direction;
    if (newIndex < 0) newIndex = 0;
    if (newIndex >= wordCount) newIndex = wordCount - 1;
    highlightCursorIndex = newIndex;
    requestUpdate();
  } else if (highlightState == HighlightState::SELECT_END) {
    // Clamp end index to this page's word count (guards against stale index after page cross)
    if (highlightEndWordIndex >= wordCount) {
      highlightEndWordIndex = wordCount > 0 ? wordCount - 1 : 0;
    }

    int newIndex = highlightEndWordIndex + direction;

    // Check if we need to go to next page
    if (newIndex >= wordCount) {
      if (section->currentPage < section->pageCount - 1) {
        section->currentPage++;
        highlightEndPage = section->currentPage;
        highlightWordCachePage = -1;  // invalidate — will rebuild on next render
        highlightEndWordIndex = 0;
        requestUpdate();
        return;
      }
      newIndex = wordCount > 0 ? wordCount - 1 : 0;
    }

    // Check if we need to go to previous page
    if (newIndex < 0) {
      if (section->currentPage > highlightStartPage) {
        section->currentPage--;
        highlightEndPage = section->currentPage;
        highlightWordCachePage = -1;  // invalidate — will rebuild on next render
        // We don't know the new page's word count yet (cache rebuilds on render),
        // so set to max int and let render clamp it
        highlightEndWordIndex = INT_MAX;
        requestUpdate();
        return;
      }
      if (section->currentPage == highlightStartPage) {
        newIndex = highlightStartWordIndex;
      } else {
        newIndex = 0;
      }
    }

    // Don't allow end to go before start on the same page
    if (section->currentPage == highlightStartPage && newIndex < highlightStartWordIndex) {
      newIndex = highlightStartWordIndex;
    }

    highlightEndWordIndex = newIndex;
    requestUpdate();
  }
}

void EpubReaderActivity::highlightMoveCursorLine(const int direction) {
  if (!section) return;

  // Use cached word list (built with correct offsets during last render)
  const auto& wordList = highlightWordCache;
  if (wordList.empty()) return;

  const bool isEnd = (highlightState == HighlightState::SELECT_END);
  const int curIdx = isEnd ? highlightEndWordIndex : highlightCursorIndex;
  if (curIdx < 0 || curIdx >= static_cast<int>(wordList.size())) return;

  const int curY = wordList[curIdx].y;
  const int curX = wordList[curIdx].x;

  // Find the target line's y value
  int targetY = -1;
  if (direction < 0) {
    for (const auto& w : wordList) {
      if (w.y < curY && (targetY < 0 || w.y > targetY)) targetY = w.y;
    }
  } else {
    for (const auto& w : wordList) {
      if (w.y > curY && (targetY < 0 || w.y < targetY)) targetY = w.y;
    }
  }

  if (targetY < 0) {
    highlightMoveCursor(direction);
    return;
  }

  // Find the word on the target line closest in x position
  int bestIdx = -1;
  int bestDist = INT_MAX;
  for (int i = 0; i < static_cast<int>(wordList.size()); i++) {
    if (wordList[i].y == targetY) {
      int dist = std::abs(wordList[i].x - curX);
      if (dist < bestDist) {
        bestDist = dist;
        bestIdx = i;
      }
    }
  }

  if (bestIdx < 0) return;

  if (isEnd && section->currentPage == highlightStartPage && bestIdx < highlightStartWordIndex) {
    bestIdx = highlightStartWordIndex;
  }

  if (isEnd) {
    highlightEndWordIndex = bestIdx;
  } else {
    highlightCursorIndex = bestIdx;
  }
  requestUpdate();
}

void EpubReaderActivity::highlightConfirmSelection() {
  if (highlightState == HighlightState::SELECT_START) {
    highlightStartSpine = currentSpineIndex;
    highlightStartPage = section->currentPage;
    highlightStartWordIndex = highlightCursorIndex;
    highlightEndPage = section->currentPage;

    // End cursor jumps to last word on page (cache was built during last render)
    highlightEndWordIndex = highlightWordCount() - 1;
    if (highlightEndWordIndex < 0) highlightEndWordIndex = 0;

    highlightState = HighlightState::SELECT_END;
    requestUpdate();
  } else if (highlightState == HighlightState::SELECT_END) {
    section->currentPage = highlightStartPage;
    highlightWordCachePage = -1;  // invalidate cache for start page
    highlightState = HighlightState::SHOW_UNDERLINE;
    highlightUnderlineStartMs = millis();
    requestUpdate();
  }
}

void EpubReaderActivity::handleHighlightInput() {
  if (!section) {
    exitHighlightMode();
    return;
  }

  // Back cancels highlight mode
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitHighlightMode();
    return;
  }

  // Confirm (release) advances highlight state.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    highlightConfirmSelection();
    return;
  }

  // Up/Down (side buttons) = move cursor up/down by line
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    highlightMoveCursorLine(-1);
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    highlightMoveCursorLine(+1);
    return;
  }

  // Left/PageBack = move cursor backward (previous word)
  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    highlightMoveCursor(-1);
    return;
  }

  // Right/PageForward = move cursor forward (next word)
  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    highlightMoveCursor(+1);
    return;
  }
}

void EpubReaderActivity::renderHighlights(const Page& page, const int fontId, const int xOffset, const int yOffset) {
  if (!section) return;
  // Rebuild cache if page changed (uses correct render offsets from renderContents)
  if (highlightWordCachePage != section->currentPage) {
    rebuildHighlightWordCache(xOffset, yOffset);
  }
  const auto& wordList = highlightWordCache;
  if (wordList.empty()) return;

  const int wordCount = static_cast<int>(wordList.size());
  const int textHeight = renderer.getTextHeight(fontId);
  constexpr int thickness = 6;

  // Clamp cursor indices to current word list size (guards against stale index after rebuild)
  if (highlightCursorIndex >= wordCount) {
    highlightCursorIndex = wordCount - 1;
  }

  // Helper: draw inverse-video cursor (black background + white text) with underline
  const auto drawCursor = [&](const WordPos& cw, const int cursorWordIdx) {
    constexpr int pad = 2;  // padding around the word for the inverse rect
    // Draw black background rect covering the word (clamp to non-negative origin)
    const int rx = (cw.x > pad) ? cw.x - pad : 0;
    const int ry = (cw.y > pad) ? cw.y - pad : 0;
    renderer.fillRect(rx, ry, cw.width + (cw.x - rx) + pad, textHeight + (cw.y - ry) + pad, true);
    // Re-draw the word text in white on top (use page ref already passed to renderHighlights)
    WordInfo wi;
    if (lookupWordInfo(page, cursorWordIdx, xOffset, yOffset, fontId, wi)) {
      renderer.drawTextSpaced(fontId, wi.x, wi.y, wi.text.c_str(), wi.letterSpacing, false, wi.style);
    }
    // Draw thick underline beneath the word
    const int underY = cw.y + textHeight + 1;
    renderer.fillRect(cw.x, underY, cw.width, thickness, true);
  };

  if (highlightState == HighlightState::SELECT_START) {
    if (highlightCursorIndex >= 0 && highlightCursorIndex < wordCount) {
      drawCursor(wordList[highlightCursorIndex], highlightCursorIndex);
    }
  } else if (highlightState == HighlightState::SELECT_END) {
    const int endIdx = highlightEndWordIndex;
    if (section->currentPage == highlightEndPage && endIdx >= 0 && endIdx < wordCount) {
      drawCursor(wordList[endIdx], endIdx);
    }
  } else if (highlightState == HighlightState::SHOW_UNDERLINE) {
    int selStart = -1;
    int selEnd = -1;

    if (section->currentPage == highlightStartPage && section->currentPage == highlightEndPage) {
      selStart = highlightStartWordIndex;
      selEnd = highlightEndWordIndex;
    } else if (section->currentPage == highlightStartPage) {
      selStart = highlightStartWordIndex;
      selEnd = wordCount - 1;
    } else if (section->currentPage == highlightEndPage) {
      selStart = 0;
      selEnd = highlightEndWordIndex;
    } else if (section->currentPage > highlightStartPage && section->currentPage < highlightEndPage) {
      selStart = 0;
      selEnd = wordCount - 1;
    }

    if (selStart >= 0 && selEnd >= 0) {
      if (selStart >= wordCount) selStart = wordCount - 1;
      if (selEnd >= wordCount) selEnd = wordCount - 1;
      // Draw continuous underline per line (no gaps between words)
      int lineY = wordList[selStart].y;
      int lineMinX = wordList[selStart].x;
      int lineMaxX = wordList[selStart].x + wordList[selStart].width;
      for (int i = selStart + 1; i <= selEnd; i++) {
        const auto& w = wordList[i];
        if (w.y != lineY) {
          // Flush previous line
          renderer.fillRect(lineMinX, lineY + textHeight + 1, lineMaxX - lineMinX, thickness, true);
          lineY = w.y;
          lineMinX = w.x;
          lineMaxX = w.x + w.width;
        } else {
          if (w.x + w.width > lineMaxX) lineMaxX = w.x + w.width;
        }
      }
      // Flush last line
      renderer.fillRect(lineMinX, lineY + textHeight + 1, lineMaxX - lineMinX, thickness, true);
    }
  }
}

std::string EpubReaderActivity::extractQuoteText() {
  if (highlightStartPage < 0 || highlightEndPage < 0 || !section) return "";
  if (highlightStartWordIndex < 0 || highlightEndWordIndex < 0) return "";

  constexpr size_t kMaxQuoteLength = 8192;
  std::string result;
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int contentY = orientedMarginTop;
  const int fontId = SETTINGS.getReaderFontId();

  for (int pg = highlightStartPage; pg <= highlightEndPage; pg++) {
    auto page = loadAndCachePage(pg);
    if (!page) continue;

    auto wordList = buildWordList(*page, orientedMarginLeft, contentY, fontId);
    if (wordList.empty()) continue;

    int startIdx = (pg == highlightStartPage) ? highlightStartWordIndex : 0;
    int endIdx = (pg == highlightEndPage) ? highlightEndWordIndex : static_cast<int>(wordList.size()) - 1;

    // Clamp to word list bounds
    if (startIdx < 0) startIdx = 0;
    if (endIdx >= static_cast<int>(wordList.size())) endIdx = static_cast<int>(wordList.size()) - 1;

    for (int i = startIdx; i <= endIdx; i++) {
      if (!result.empty()) {
        // Check if the word starts with punctuation that should be attached
        const char first = wordList[i].text.empty() ? '\0' : wordList[i].text[0];
        if (first != ',' && first != '.' && first != ';' && first != ':' && first != '!' && first != '?' &&
            first != ')' && first != '"') {
          result += ' ';
        }
      }
      result += wordList[i].text;
      if (result.size() >= kMaxQuoteLength) break;
    }
    if (result.size() >= kMaxQuoteLength) break;
  }

  return result;
}

std::string EpubReaderActivity::getChapterTitle() const {
  if (!epub) return "";
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex >= 0) {
    const auto tocEntry = epub->getTocItem(tocIndex);
    return tocEntry.title;
  }
  return "Chapter " + std::to_string(currentSpineIndex + 1);
}

void EpubReaderActivity::saveQuoteToFile(const std::string& quote) {
  if (!epub || quote.empty()) return;

  // Build quotes file path: same directory as book, with _QUOTES.txt suffix
  std::string bookPath = epub->getPath();
  // Find last dot to strip extension
  auto dotPos = bookPath.rfind('.');
  std::string basePath = (dotPos != std::string::npos) ? bookPath.substr(0, dotPos) : bookPath;
  std::string quotesPath = basePath + "_QUOTES.txt";

  // Open file in append mode
  HalFile file = Storage.open(quotesPath.c_str(), O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("HLT", "Failed to open quotes file: %s", quotesPath.c_str());
    return;
  }

  // Write chapter info and quote
  std::string chapterTitle = getChapterTitle();
  std::string entry = "[" + chapterTitle + "]\n" + quote + "\n---\n\n";
  file.write(entry.c_str(), entry.size());
  file.close();

  LOG_DBG("HLT", "Quote saved to %s", quotesPath.c_str());
}

// ── End Highlight / Quote selection mode ─────────────────────────────────────

void EpubReaderActivity::renderContents(const Page& page, const int orientedMarginTop, const int orientedMarginRight,
                                        const int orientedMarginBottom, const int orientedMarginLeft,
                                        const StatusBarLayout& statusBarLayout) {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.setTextRenderStyle(SETTINGS.textRenderMode);

  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const int contentY = orientedMarginTop;

  // Two-pass font prewarm: scan pass collects text, then decompress needed glyphs.
  // The actual render must happen inside the scope so page buffers stay alive.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    auto scope = fcm->createPrewarmScope();
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);  // scan pass
    scope.endScanAndPrewarm();
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);  // actual render
  } else {
    page.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);
  }
  // Render highlight overlay and border if in highlight/quote selection mode
  if (highlightState != HighlightState::NONE) {
    renderHighlights(page, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);
    // Draw solid border around text area to indicate highlight mode
    const int border = 6;
    const int bx = orientedMarginLeft - border;
    const int by = contentY - border;
    const int bw = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight + 2 * border;
    const int bh = viewportHeight + 2 * border;
    renderer.drawRect(bx, by, bw, bh, border, true);
  }

  if (SETTINGS.debugBorders) {
    DrawUtils::drawDottedRect(renderer, orientedMarginLeft, orientedMarginTop,
                              renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight, viewportHeight);
  }

  renderStatusBar(statusBarLayout, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

  const bool pageHasImages = page.hasImages();

  if (pagesUntilFullRefresh <= 1 || pageHasImages) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  // Apply hardware grayscale overlay for pages with images.
  // This uses the same LSB/MSB technique as sleep wallpapers to render
  // true 4-level grayscale, making photographs much more visible on e-ink.
  if (pageHasImages && renderer.storeBwBuffer()) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    page.renderImages(renderer, orientedMarginLeft, contentY);
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    page.renderImages(renderer, orientedMarginLeft, contentY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.restoreBwBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  renderer.setTextRenderStyle(0);
}

void EpubReaderActivity::renderStatusBar(const StatusBarLayout& statusBarLayout, const int orientedMarginRight,
                                         const int orientedMarginBottom, const int orientedMarginLeft) {
  ReaderStatusBar::renderStatusBar(renderer, statusBarLayout, orientedMarginRight,
                                   orientedMarginBottom, orientedMarginLeft, SETTINGS.debugBorders);
}

void EpubReaderActivity::navigateToHref(const char* href, const bool savePosition) {
  if (!epub || !href) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string hrefStr(href);

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", href);
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, href);
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}
