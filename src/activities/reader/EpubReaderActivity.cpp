#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <new>
#include <vector>

#include "BookmarkListActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "QuotesViewerActivity.h"
#include "ReaderLayoutSafety.h"
#include "ReadingThemeStore.h"
#include "ReadingThemesActivity.h"
#include "RecentBooksStore.h"
#include "activities/network/QRShareActivity.h"
#include "activities/util/ConfirmDialogActivity.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "persist/BackupMirror.h"
#include "util/DrawUtils.h"
#include "util/FavoriteBmp.h"
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

using ReaderStatusBar::computeStatusBarReservedHeight;
using ReaderStatusBar::computeStatusBarsHeight;
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

  // Block 2 (v1.2.0): half refresh on book enter is enough to scrub the
  // library list or file-actions menu ghost and is ~1 s faster than FULL.
  // Downgrade is experimental — revert to requestFullRefresh() if ghost
  // artifacts appear under the first page.
  renderer.requestHalfRefresh();

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  applyReaderOrientation(renderer, SETTINGS.orientation);
  EpdFontFamily::setReaderBoldSwapEnabled(SETTINGS.readerBoldSwap != 0);
  ImageBlock::setDitherMode(SETTINGS.imageDither);

  epub->setupCacheDir();

  int32_t loadedPageCount = -1;
  FsFile f;
  const std::string progPath = epub->getCachePath() + "/progress.bin";
  const std::string bakPath = epub->getCachePath() + "/progress.bin.bak";
  bool opened = Storage.openFileForRead("ERS", progPath, f);
  if (!opened && Storage.exists(bakPath.c_str())) {
    LOG_INF("ERS", "progress.bin missing, recovering from progress.bin.bak");
    opened = Storage.openFileForRead("ERS", bakPath, f);
  }
  if (!opened) {
    // Last resort: try /.crosspoint/backups/ mirror
    const std::string flatName = backup::flatNameForCacheFile(epub->getCachePath(), "progress.bin");
    if (backup::restoreFromMirror(flatName, progPath)) {
      LOG_INF("ERS", "progress.bin recovered from mirror %s", flatName.c_str());
      opened = Storage.openFileForRead("ERS", progPath, f);
    }
  }
  if (opened) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      loadedPageCount = cachedChapterTotalPageCount;
    }
    f.close();
  }
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount > 0 && currentSpineIndex >= spineCount) {
    currentSpineIndex = spineCount - 1;
    nextPageNumber = UINT16_MAX;
  }

  progressSink_.setCachePath(epub->getCachePath());
  progressSink_.setSpineCount(spineCount);
  progress_.setDebounceMs(progressSaveDebounceMs);
  progress_.seed({static_cast<int32_t>(currentSpineIndex), static_cast<int32_t>(nextPageNumber), loadedPageCount});

  // We may want a better condition to detect if we are opening for the first
  // time. This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());

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
  highlights_.exit();
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

void EpubReaderActivity::invalidateStatusBarCaches() { statusBarCache_.clear(); }

void EpubReaderActivity::clearPageCache() { cache_.detach(); }

std::shared_ptr<Page> EpubReaderActivity::getCachedPage(const int pageIndex) const {
  return cache_.get(pageIndex, currentSpineIndex);
}

std::shared_ptr<Page> EpubReaderActivity::loadAndCachePage(const int pageIndex) {
  if (!section) {
    return {};
  }

  // refreshPageCacheWindow calls this per page in the prefetch window, so
  // on long first-opens it fires often enough to let the 2s reassurance
  // repaint blink the "Opening book..." popup through the wait.
  TransitionFeedback::maybeShowStillWorkingToast(renderer);

  auto page = std::shared_ptr<Page>(section->loadPageFromSectionFile(pageIndex));
  if (!page) {
    return {};
  }

  if (cache_.spineIndex() != currentSpineIndex) {
    cache_.attach(currentSpineIndex);
  }
  cache_.insert(pageIndex, page);
  return page;
}

void EpubReaderActivity::refreshPageCacheWindow(const int centerPage, const std::shared_ptr<Page>& currentPage) {
  if (!section || centerPage < 0 || centerPage >= section->pageCount) {
    cache_.clear();
    return;
  }

  if (cache_.spineIndex() != currentSpineIndex) {
    cache_.attach(currentSpineIndex);
  }

  cache_.refreshWindow(centerPage, currentPage, section->pageCount, [this](int pageIndex) {
    return std::shared_ptr<Page>(section->loadPageFromSectionFile(pageIndex));
  });
}

int EpubReaderActivity::getWrappedStatusBarReserveLineCount(const int usableWidth) {
  if (!epub || usableWidth <= 0) {
    return 1;
  }
  // Composite cache key: (spineIndex, usableWidth, noTitleTruncation). All three must match for
  // the cached reserve-line count to be valid.
  //   spineIndex          — the TOC title set changes per chapter.
  //   usableWidth         — orientation flip or margin changes reflow the wrap.
  //   noTitleTruncation   — toggling the setting changes the line-count policy, not the width.
  // Any mismatch forces a re-measure, which is expensive (see kMaxTocTitlesMeasured below).
  if (statusBarCache_.cachedReserveSpineIndex == currentSpineIndex &&
      statusBarCache_.cachedReserveUsableWidth == usableWidth &&
      statusBarCache_.cachedReserveNoTitleTruncation == SETTINGS.statusBarNoTitleTruncation) {
    return statusBarCache_.cachedReserveTitleLineCount;
  }

  int maxLines = 1;
  auto tocIndexes = epub->getTocIndexesForSpineIndex(currentSpineIndex);
  if (tocIndexes.empty()) {
    const int fallbackIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (fallbackIndex >= 0) {
      tocIndexes.push_back(fallbackIndex);
    }
  }

  // Cap the sample size. Books like Pocket Oracle have hundreds of TOC
  // entries per spine (one per aphorism). Measuring each title against a
  // cold font cache on first-open costs ~140 ms per entry — on a 300-entry
  // spine that's 40+ seconds of blocking work while "Opening book..." sits
  // invisible on screen. The first handful of entries are representative
  // of the max wrap count for the rest, so cap the iteration and move on.
  constexpr size_t kMaxTocTitlesMeasured = 8;
  const size_t toMeasure = std::min(tocIndexes.size(), kMaxTocTitlesMeasured);
  for (size_t i = 0; i < toMeasure; ++i) {
    const int tocIndex = tocIndexes[i];
    const std::string title = epub->formatTocDisplayTitle(tocIndex);
    if (title.empty()) {
      continue;
    }
    const int lineCount =
        static_cast<int>(wrapStatusText(renderer, SETTINGS.getStatusBarFontId(), title, usableWidth).size());
    if (lineCount > maxLines) {
      maxLines = lineCount;
    }
  }

  statusBarCache_.cachedReserveSpineIndex = currentSpineIndex;
  statusBarCache_.cachedReserveUsableWidth = usableWidth;
  statusBarCache_.cachedReserveNoTitleTruncation = SETTINGS.statusBarNoTitleTruncation;
  statusBarCache_.cachedReserveTitleLineCount = maxLines;
  return statusBarCache_.cachedReserveTitleLineCount;
}

const std::vector<std::string>& EpubReaderActivity::getStatusBarTitleLines(const int tocIndex, const int usableWidth,
                                                                           const bool noTitleTruncation,
                                                                           const int maxTitleLineCount) {
  if (statusBarCache_.cachedTitleTocIndex == tocIndex && statusBarCache_.cachedTitleUsableWidth == usableWidth &&
      statusBarCache_.cachedTitleNoTitleTruncation == noTitleTruncation &&
      statusBarCache_.cachedTitleMaxLines == maxTitleLineCount) {
    return statusBarCache_.cachedTitleLines;
  }

  std::string titleText = tr(STR_UNNAMED);
  if (tocIndex >= 0 && epub) {
    titleText = epub->formatTocDisplayTitle(tocIndex);
    if (titleText.empty()) {
      titleText = tr(STR_UNNAMED);
    }
  }

  statusBarCache_.cachedTitleLines = ReaderLayoutSafety::buildTitleLines(
      renderer, SETTINGS.getStatusBarFontId(), titleText, usableWidth, noTitleTruncation, maxTitleLineCount);

  statusBarCache_.cachedTitleTocIndex = tocIndex;
  statusBarCache_.cachedTitleUsableWidth = usableWidth;
  statusBarCache_.cachedTitleNoTitleTruncation = noTitleTruncation;
  statusBarCache_.cachedTitleMaxLines = maxTitleLineCount;
  return statusBarCache_.cachedTitleLines;
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
  if (!SETTINGS.statusBarEnabled || !section || !epub) {
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
    layout.bookPercentageTextWidth =
        renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.bookPercentageText.c_str());
  }
  if (SETTINGS.statusBarShowChapterPercentage) {
    char chapterPercentageStr[16] = {0};
    snprintf(chapterPercentageStr, sizeof(chapterPercentageStr), "C:%.0f%%", layout.chapterProgress);
    layout.chapterPercentageText = chapterPercentageStr;
    layout.chapterPercentageTextWidth =
        renderer.getTextWidth(SETTINGS.getStatusBarFontId(), layout.chapterPercentageText.c_str());
  }

  populateBookPageCounterText(layout);

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

void EpubReaderActivity::populateBookPageCounterText(StatusBarLayout& layout) const {
  if (!SETTINGS.statusBarShowBookPageCounter || !section || !epub || section->pageCount <= 0) {
    return;
  }
  // Estimate total book pages by extrapolating the current chapter's pages-per-byte ratio to
  // the whole book. Approximate by design: chapters with images or code have different density
  // than prose, so the estimated total can fluctuate as the reader moves between chapter types.
  // We accept that drift rather than pre-indexing every chapter (which would block first-open).
  const size_t bookSize = epub->getBookSize();
  const size_t prevChapterSize =
      (currentSpineIndex >= 1) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = epub->getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  if (curChapterSize == 0 || bookSize == 0) {
    return;
  }
  const float pagesPerByte = static_cast<float>(section->pageCount) / static_cast<float>(curChapterSize);
  const int totalEstimatedPages = std::max(1, static_cast<int>(pagesPerByte * static_cast<float>(bookSize) + 0.5f));
  const int currentAbsPage = std::max(
      1, std::min(totalEstimatedPages,
                  static_cast<int>(layout.bookProgress / 100.0f * static_cast<float>(totalEstimatedPages) + 0.5f)));
  char buf[32];
  snprintf(buf, sizeof(buf), "%d/%d", currentAbsPage, totalEstimatedPages);
  layout.bookPageCounterText = buf;
  layout.bookPageCounterTextWidth = renderer.getTextWidth(SETTINGS.getStatusBarFontId(), buf);
}

void EpubReaderActivity::loop() {
  flushProgressIfNeeded(false);

  if (subActivity) {
    loopSubActivity();
    return;
  }

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
  if (pendingSectionReset) {
    pendingSectionReset = false;
    section.reset();
  }

  // Highlight mode intercepts all input while active
  if (highlights_.state() != HighlightState::NONE) {
    loopHighlightMode();
    return;
  }

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
  // SHOW_UNDERLINE: wait 3 seconds (HighlightController::kUnderlineTimeoutMs)
  // then save quote and exit.
  if (highlights_.state() == HighlightState::SHOW_UNDERLINE) {
    if (highlights_.underlineTimedOut(millis())) {
      std::string quote = extractQuoteText();
      if (!quote.empty()) {
        saveQuoteToFile(quote);
        StatusPopup::showConfirmation(renderer, "Quote saved!");
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
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  if (skipChapter) {
    TransitionFeedback::show(renderer, tr(STR_LOADING));
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      saveProgress(currentSpineIndex, nextPageNumber, 1);
      clearPageCache();
      section.reset();
    }
    requestUpdate();
    return;
  }

  // No current section — user interaction triggers a rebuild attempt.
  if (!section) {
    if (mappedInput.wasAnyPressed()) {
      requestUpdate();
    }
    return;
  }

  if (prevTriggered) {
    if (section->currentPage > 0) {
      section->currentPage--;
      flushProgressIfNeeded(true);
    } else if (currentSpineIndex > 0) {
      TransitionFeedback::show(renderer, tr(STR_LOADING));
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        saveProgress(currentSpineIndex, nextPageNumber, 1);
        clearPageCache();
        section.reset();
      }
    }
    requestUpdate();
  } else {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
      addSessionPagesRead();
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
  const int pageNum = section ? section->currentPage : 0;
  const bool isBookmarked = bookmarkStore.has(currentSpineIndex, pageNum);
  const int bmCount = bookmarkStore.count();
  const std::string quotesPath = getQuotesFilePath();
  const bool hasQuotes = !quotesPath.empty() && Storage.exists(quotesPath.c_str());
  exitActivity();
  enterNewActivity(new EpubReaderMenuActivity(
      this->renderer, this->mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
      SETTINGS.orientation, !currentPageFootnotes.empty(), isBookmarked, bmCount, hasQuotes,
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
    case EpubReaderMenuActivity::MenuAction::HIGHLIGHT_QUOTE: {
      exitActivity();
      enterHighlightMode();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_QUOTES: {
      const std::string quotesPath = getQuotesFilePath();
      if (quotesPath.empty() || !Storage.exists(quotesPath.c_str())) {
        exitActivity();
        requestUpdate();
        break;
      }
      exitActivity();
      enterNewActivity(new QuotesViewerActivity(this->renderer, this->mappedInput, quotesPath,
                                                [this] { pendingSubactivityExit = true; }));
      break;
    }
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
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      const int pageNum = section ? section->currentPage : 0;
      const bool alreadyExists = bookmarkStore.has(currentSpineIndex, pageNum);
      if (!alreadyExists && bookmarkStore.count() >= BookmarkStore::MAX_BOOKMARKS) {
        // Full — show brief feedback then return to menu
        StatusPopup::showConfirmation(renderer, "Bookmarks full (20 max)");
        exitActivity();
        requestUpdate();
        break;
      }
      bookmarkStore.toggle(currentSpineIndex, pageNum);
      bookmarkStore.save(epub->getCachePath());
      exitActivity();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_LIST: {
      const int spineIdx = currentSpineIndex;
      const int pageNum = section ? section->currentPage : 0;
      exitActivity();
      enterNewActivity(new BookmarkListActivity(
          this->renderer, this->mappedInput, epub, bookmarkStore, epub->getCachePath(), spineIdx, pageNum,
          [this] {
            exitActivity();
            requestUpdate();
          },
          [this](const int targetSpine, const int targetPage) {
            // Save current position for "go back" (reuse footnote stack)
            if (section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
              savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
              footnoteDepth++;
            }
            // Jump to bookmark
            if (currentSpineIndex != targetSpine || !section || section->currentPage != targetPage) {
              TransitionFeedback::show(renderer, tr(STR_LOADING));
              currentSpineIndex = targetSpine;
              nextPageNumber = targetPage;
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
        StatusPopup::showConfirmation(renderer, FavoriteBmp::limitReachedPopupMessage());
      } else if (result == FavoriteBmp::SetFavoriteResult::RenameConflict) {
        StatusPopup::showConfirmation(renderer, "Favorite name already exists");
      } else if (result != FavoriteBmp::SetFavoriteResult::Success) {
        StatusPopup::showConfirmation(renderer, "Favorite failed");
      } else {
        StatusPopup::showConfirmation(renderer, makeFavorite ? "Favorited" : "Unfavorited");
      }
      exitActivity();
      pendingMenuOpen = false;
      // Input suppression handled by exitActivity()
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_PAUSE_ROTATION: {
      APP_STATE.wallpaperRotationPaused = !APP_STATE.wallpaperRotationPaused;
      APP_STATE.saveToFile();
      StatusPopup::showConfirmation(renderer,
                                    APP_STATE.wallpaperRotationPaused ? "Rotation paused" : "Rotation unpaused");
      exitActivity();
      pendingMenuOpen = false;
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRIAGE_MOVE_PAUSE: {
      const std::string lastPath = APP_STATE.lastSleepWallpaperPath;
      if (lastPath.empty()) break;
      if (lastPath.rfind("/sleep pause/", 0) == 0) {
        StatusPopup::showConfirmation(renderer, "Already in sleep pause");
        exitActivity();
        pendingMenuOpen = false;
        // Input suppression handled by exitActivity()
        requestUpdate();
        break;
      }
      const std::string destDir = "/sleep pause";
      Storage.mkdir(destDir.c_str());
      const auto slashPos = lastPath.find_last_of('/');
      const std::string filename = (slashPos == std::string::npos) ? lastPath : lastPath.substr(slashPos + 1);
      const std::string dstPath = destDir + "/" + filename;
      FsFile src, dst;
      bool ok = false;
      if (Storage.openFileForRead("TRG", lastPath.c_str(), src) &&
          Storage.openFileForWrite("TRG", dstPath.c_str(), dst)) {
        uint8_t buf[512];
        ok = true;
        while (src.available()) {
          const int n = src.read(buf, sizeof(buf));
          if (n <= 0 || dst.write(buf, n) != n) {
            ok = false;
            break;
          }
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
      StatusPopup::showConfirmation(renderer, ok ? "Moved to sleep pause" : "Move failed");
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
      StatusPopup::showConfirmation(renderer, removed ? "Wallpaper deleted" : "Delete failed");
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
      const std::string bookTitle = epub ? epub->getTitle() : "";
      exitActivity();
      enterNewActivity(new ConfirmDialogActivity(
          renderer, mappedInput, "Delete from device?\n" + bookTitle,
          [this]() {
            exitActivity();
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
          },
          [this]() {
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::REMOVE_FROM_RECENT: {
      const std::string bookTitle = epub ? epub->getTitle() : "";
      exitActivity();
      enterNewActivity(new ConfirmDialogActivity(
          renderer, mappedInput, "Remove from recents?\n" + bookTitle,
          [this]() {
            exitActivity();
            if (epub) {
              RECENT_BOOKS.removeBook(epub->getPath());
              if (APP_STATE.openEpubPath == epub->getPath()) {
                APP_STATE.openEpubPath = "";
                APP_STATE.saveToFile();
              }
            }
            pendingGoHome = true;
          },
          [this]() {
            exitActivity();
            requestUpdate();
          }));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SHARE_QR: {
      if (epub) {
        exitActivity();
        enterNewActivity(new QRShareActivity(
            renderer, mappedInput,
            [this] {
              exitActivity();
              requestUpdate();
            },
            epub->getPath()));
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
    }
    invalidateStatusBarCaches();
    clearPageCache();
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::render(Activity::RenderLock&& lock) {
  if (!epub || pendingSectionReset) {
    return;
  }

  // Long first-open renders (cached sections on big books) spend seconds
  // inside loadSectionFile + page draw with no other tick points. Fire the
  // reassurance repaint here so the "Opening book..." popup gets refreshed
  // on the 10-second cadence even when createSectionFile's layout ticks
  // don't run.
  TransitionFeedback::maybeShowStillWorkingToast(renderer);

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

  // Apply screen viewable areas and additional padding.
  // Downstream helpers still take the four scalars individually, so we unpack the struct into
  // locals at this boundary. Status-bar reserve is applied further down after resolveStatusBarBudget.
  const auto baseMargins = ReaderLayoutSafety::resolveBaseReaderMargins(
      renderer, SETTINGS.screenMarginTop, SETTINGS.screenMarginBottom, SETTINGS.screenMarginHorizontal,
      SETTINGS.dynamicMargins, SETTINGS.getReaderFontId());
  int orientedMarginTop = baseMargins.top;
  int orientedMarginRight = baseMargins.right;
  int orientedMarginBottom = baseMargins.bottom;
  int orientedMarginLeft = baseMargins.left;
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
         statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter && statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition));
    const bool showBottomStatusTextRow =
        (SETTINGS.statusBarShowBattery && !statusTextPositionIsTop(SETTINGS.statusBarBatteryPosition)) ||
        (SETTINGS.statusBarShowPageCounter && !statusTextPositionIsTop(SETTINGS.statusBarPageCounterPosition)) ||
        (SETTINGS.statusBarShowBookPercentage && !statusTextPositionIsTop(SETTINGS.statusBarBookPercentagePosition)) ||
        (SETTINGS.statusBarShowChapterPercentage &&
         !statusTextPositionIsTop(SETTINGS.statusBarChapterPercentagePosition)) ||
        (SETTINGS.statusBarShowBookPageCounter && !statusTextPositionIsTop(SETTINGS.statusBarBookPageCounterPosition));
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
        renderer, SETTINGS.getStatusBarFontId(), "ERS", renderer.getScreenHeight(), getStatusTopInset(renderer),
        getStatusBottomInset(renderer), SETTINGS.screenMarginTop, SETTINGS.screenMarginBottom, minContentHeight,
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
    auto* sec = new (std::nothrow) Section(epub, currentSpineIndex, renderer);
    if (!sec) {
      LOG_ERR("ERS", "OOM: Section allocation");
      return;
    }
    section = std::unique_ptr<Section>(sec);
    bool builtSection = false;
    clearPageCache();

    const uint8_t sectionTextRenderMode = SETTINGS.textRenderMode;

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                  SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                  SETTINGS.readerBoldSwap != 0)) {
      LOG_DBG("ERS", "Cache not found, building...");
      builtSection = true;

      // Free font caches to reclaim contiguous heap for ZIP decompression
      // (needs a 32 KB dictionary). They rebuild automatically on next render.
      auto* fcm = renderer.getFontCacheManager();
      if (fcm) fcm->clearCache();

      // Progress hook: parser emits percentages as it chews the HTML. We
      // only use it to gate the "Still working on it..." toast, not a real
      // bar — keeps UX simple and consistent with cached-open behaviour.
      auto layoutProgressTick = [this](int) { TransitionFeedback::maybeShowStillWorkingToast(renderer); };
      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                      SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                      SETTINGS.readerBoldSwap != 0, layoutProgressTick)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        clearPageCache();
        section.reset();
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 350, "Out of memory", true, EpdFontFamily::REGULAR);
        renderer.drawCenteredText(UI_12_FONT_ID, 400, "Press Back to exit", true, EpdFontFamily::REGULAR);
        renderer.displayBuffer();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    // CSS rules are only needed during section creation (layout).  Free them
    // now to reclaim ~100 KB for page rendering and font decompression.
    // They reload from cache automatically if a new section needs building.
    {
      auto* css = epub->getCssParser();
      if (css) css->clear();
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

    // One more chance to blink "Opening book..." before section-init
    // dismiss — catches slow loadSectionFile on big cached sections.
    TransitionFeedback::maybeShowStillWorkingToast(renderer);

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
      // Defer section.reset() to loop() — it will rebuild and re-render.
      // Do NOT requestUpdate() here: the render task would race with loop()
      // resetting section, causing a null dereference in buildStatusBarLayout.
      pendingSectionReset = true;
      if (pageLoadFailCount >= 3) {
        LOG_ERR("ERS", "Page load failed %d times, showing error", pageLoadFailCount);
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, "Page load failed", true, EpdFontFamily::REGULAR);
        renderer.displayBuffer();
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
  flushProgressIfNeeded(false);  // observes current render position + debounce-flushes
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
                                  viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                  SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                  SETTINGS.readerBoldSwap != 0)) {
    return;  // Already cached
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacingLevel, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled != 0, SETTINGS.wordSpacingPercent,
                                     SETTINGS.firstLineIndentMode, SETTINGS.readerStyleMode, sectionTextRenderMode,
                                     SETTINGS.readerBoldSwap != 0)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  // Force-write a specific position (chapter skip, orientation change, etc.)
  // and align the tracker so the next debounced flush doesn't rewrite stale
  // values. Sink owns the 6-byte atomic-write format + bounds clamping.
  const crosspoint::reader::ReaderPosition pos{static_cast<int32_t>(spineIndex), static_cast<int32_t>(currentPage),
                                               static_cast<int32_t>(pageCount)};
  progressSink_.write(pos);
  progress_.seed(pos);
}
void EpubReaderActivity::flushProgressIfNeeded(const bool force) {
  if (!epub || !section || section->pageCount == 0) {
    return;
  }
  const auto now = millis();
  progress_.observe({static_cast<int32_t>(currentSpineIndex), static_cast<int32_t>(section->currentPage),
                     static_cast<int32_t>(section->pageCount)},
                    now);
  progress_.flush(now, force);
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

void EpubReaderActivity::rebuildHighlightWordCache(const int xOffset, const int yOffset) {
  std::vector<crosspoint::reader::WordPos> words;
  auto page = loadAndCachePage(section->currentPage);
  if (page) {
    const int fontId = SETTINGS.getReaderFontId();
    for (const auto& el : page->elements) {
      if (el->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*el);
      const auto& tb = line.getTextBlock();
      const auto& wordsRef = tb.getWords();
      const auto& xpos = tb.getWordXpos();
      const auto& styles = tb.getWordStyles();
      const int16_t ls = tb.getLetterSpacing();
      for (size_t i = 0; i < wordsRef.size(); i++) {
        crosspoint::reader::WordPos wp;
        wp.x = static_cast<int16_t>(static_cast<int>(xpos[i]) + line.xPos + xOffset);
        wp.y = static_cast<int16_t>(line.yPos + yOffset);
        wp.width = static_cast<int16_t>(renderer.getTextWidthSpaced(fontId, wordsRef[i].c_str(), ls, styles[i]));
        words.push_back(wp);
      }
    }
  }
  highlights_.setWordsForPage(section->currentPage, std::move(words));
}

void EpubReaderActivity::enterHighlightMode() {
  if (!section || section->pageCount == 0) return;
  highlights_.enter();
  requestUpdate();
}

void EpubReaderActivity::exitHighlightMode() {
  highlights_.exit();
  requestUpdate();
}

void EpubReaderActivity::highlightMoveCursor(const int direction) {
  if (!section) return;
  const crosspoint::reader::PageContext ctx{section->currentPage, section->pageCount, highlights_.wordCount()};
  const auto r = highlights_.moveCursor(direction, ctx);
  if (r.pageDelta != 0) section->currentPage += r.pageDelta;
  if (r.stateChanged) requestUpdate();
}

void EpubReaderActivity::highlightMoveCursorLine(const int direction) {
  if (!section) return;
  const crosspoint::reader::PageContext ctx{section->currentPage, section->pageCount, highlights_.wordCount()};
  const auto r = highlights_.moveCursorLine(direction, ctx);
  if (r.pageDelta != 0) section->currentPage += r.pageDelta;
  if (r.stateChanged) requestUpdate();
}

void EpubReaderActivity::highlightConfirmSelection() {
  if (!section) return;
  const auto r = highlights_.confirm(currentSpineIndex, section->currentPage, millis());
  if (r.pageDelta != 0) section->currentPage += r.pageDelta;
  if (r.stateChanged) requestUpdate();
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
  if (!highlights_.wordCacheValidFor(section->currentPage)) {
    rebuildHighlightWordCache(xOffset, yOffset);
  }
  const auto& wordList = highlights_.words();
  if (wordList.empty()) return;

  const int wordCount = static_cast<int>(wordList.size());
  const int textHeight = renderer.getTextHeight(fontId);
  constexpr int thickness = 2;  // SHOW_UNDERLINE dashed underline thickness

  const int cursorIdx = highlights_.cursorIndex() >= wordCount ? wordCount - 1 : highlights_.cursorIndex();

  // Helper: draw cursor as a solid black rectangle with the word text redrawn
  // in white (inverted). Only used during SELECT_START / SELECT_END — the
  // dashed underline appears later during SHOW_UNDERLINE confirmation.
  // `wi` carries text + style for the inverted glyph redraw; `cw` carries the
  // cached geometry used for the black-fill rect.
  const auto drawCursor = [&](const crosspoint::reader::WordPos& cw, const WordInfo* wi) {
    constexpr int pad = 2;  // breathing room between word glyphs and black fill
    const int bx = (cw.x > pad) ? cw.x - pad : 0;
    const int by = (cw.y > pad) ? cw.y - pad : 0;
    const int bw = cw.width + (cw.x - bx) + pad;
    const int bh = textHeight + (cw.y - by) + pad;
    renderer.fillRect(bx, by, bw, bh, true);
    if (wi != nullptr && !wi->text.empty()) {
      renderer.drawTextSpaced(fontId, wi->x, wi->y, wi->text.c_str(), wi->letterSpacing, false, wi->style);
    }
  };

  const auto state = highlights_.state();
  const bool needsCursorText = (state == HighlightState::SELECT_START || state == HighlightState::SELECT_END);
  // buildWordList gives us text + style for white-text redraw on the cursor word.
  std::vector<WordInfo> infoList;
  if (needsCursorText) infoList = buildWordList(page, xOffset, yOffset, fontId);
  const auto wordInfoAt = [&](int idx) -> const WordInfo* {
    if (idx < 0 || idx >= static_cast<int>(infoList.size())) return nullptr;
    return &infoList[idx];
  };

  if (state == HighlightState::SELECT_START) {
    if (cursorIdx >= 0 && cursorIdx < wordCount) {
      drawCursor(wordList[cursorIdx], wordInfoAt(cursorIdx));
    }
  } else if (state == HighlightState::SELECT_END) {
    const int endIdx = highlights_.endWordIndex();
    if (section->currentPage == highlights_.endPage() && endIdx >= 0 && endIdx < wordCount) {
      drawCursor(wordList[endIdx], wordInfoAt(endIdx));
    }
  } else if (state == HighlightState::SHOW_UNDERLINE) {
    const int startPage = highlights_.startPage();
    const int endPage = highlights_.endPage();
    const int startWord = highlights_.startWordIndex();
    const int endWord = highlights_.endWordIndex();
    int selStart = -1;
    int selEnd = -1;

    if (section->currentPage == startPage && section->currentPage == endPage) {
      selStart = startWord;
      selEnd = endWord;
    } else if (section->currentPage == startPage) {
      selStart = startWord;
      selEnd = wordCount - 1;
    } else if (section->currentPage == endPage) {
      selStart = 0;
      selEnd = endWord;
    } else if (section->currentPage > startPage && section->currentPage < endPage) {
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
          drawDashedHLine(renderer, lineMinX, lineY + textHeight + 1, lineMaxX - lineMinX, thickness);
          lineY = w.y;
          lineMinX = w.x;
          lineMaxX = w.x + w.width;
        } else {
          if (w.x + w.width > lineMaxX) lineMaxX = w.x + w.width;
        }
      }
      // Flush last line
      drawDashedHLine(renderer, lineMinX, lineY + textHeight + 1, lineMaxX - lineMinX, thickness);
    }
  }
}

std::string EpubReaderActivity::extractQuoteText() {
  const int startPage = highlights_.startPage();
  const int endPage = highlights_.endPage();
  const int startWord = highlights_.startWordIndex();
  const int endWord = highlights_.endWordIndex();
  if (startPage < 0 || endPage < 0 || !section) return "";
  if (startWord < 0 || endWord < 0) return "";

  constexpr size_t kMaxQuoteLength = 8192;
  std::string result;
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int contentY = orientedMarginTop;
  const int fontId = SETTINGS.getReaderFontId();

  for (int pg = startPage; pg <= endPage; pg++) {
    auto page = loadAndCachePage(pg);
    if (!page) continue;

    auto wordList = buildWordList(*page, orientedMarginLeft, contentY, fontId);
    if (wordList.empty()) continue;

    int startIdx = (pg == startPage) ? startWord : 0;
    int endIdx = (pg == endPage) ? endWord : static_cast<int>(wordList.size()) - 1;

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

std::string EpubReaderActivity::getQuotesFilePath() const {
  if (!epub) return "";
  const std::string bookPath = epub->getPath();
  const auto dotPos = bookPath.rfind('.');
  const std::string basePath = (dotPos != std::string::npos) ? bookPath.substr(0, dotPos) : bookPath;
  return basePath + "_QUOTES.txt";
}

void EpubReaderActivity::saveQuoteToFile(const std::string& quote) {
  if (!epub || quote.empty()) return;

  const std::string quotesPath = getQuotesFilePath();
  const std::string tmpPath = quotesPath + ".tmp";
  const std::string bakPath = quotesPath + ".bak";

  // Atomic read-modify-write: copy existing primary into .tmp, append the new
  // entry, then rotate primary -> .bak and .tmp -> primary. A torn write or
  // power loss leaves .bak as the prior good state.
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  HalFile dst;
  if (!Storage.openFileForWrite("HLT", tmpPath, dst)) {
    LOG_ERR("HLT", "Failed to open quotes tmp for writing: %s", tmpPath.c_str());
    return;
  }

  // Copy existing content (if any) into tmp
  if (Storage.exists(quotesPath.c_str())) {
    HalFile src;
    if (Storage.openFileForRead("HLT", quotesPath, src)) {
      uint8_t buffer[512];
      while (src.available()) {
        const int rd = src.read(buffer, sizeof(buffer));
        if (rd <= 0) break;
        if (dst.write(buffer, rd) != static_cast<size_t>(rd)) {
          LOG_ERR("HLT", "Failed to copy existing quotes into tmp");
          src.close();
          dst.close();
          Storage.remove(tmpPath.c_str());
          return;
        }
      }
      src.close();
    }
  }

  // Append new entry
  const std::string chapterTitle = getChapterTitle();
  const std::string entry = "[" + chapterTitle + "]\n" + quote + "\n---\n\n";
  if (dst.write(entry.c_str(), entry.size()) != entry.size()) {
    LOG_ERR("HLT", "Failed to append new quote to tmp");
    dst.close();
    Storage.remove(tmpPath.c_str());
    return;
  }
  dst.flush();
  dst.close();

  // 2-layer rotation
  if (Storage.exists(bakPath.c_str())) {
    if (!Storage.remove(bakPath.c_str())) {
      LOG_ERR("HLT", "Failed to remove stale quotes bak %s", bakPath.c_str());
    }
  }
  if (Storage.exists(quotesPath.c_str())) {
    if (!Storage.rename(quotesPath.c_str(), bakPath.c_str())) {
      LOG_ERR("HLT", "Failed to rotate %s -> %s", quotesPath.c_str(), bakPath.c_str());
      Storage.remove(tmpPath.c_str());
      return;
    }
  }
  if (!Storage.rename(tmpPath.c_str(), quotesPath.c_str())) {
    LOG_ERR("HLT", "Failed to promote quotes tmp to %s", quotesPath.c_str());
    if (Storage.exists(bakPath.c_str())) {
      if (Storage.rename(bakPath.c_str(), quotesPath.c_str())) {
        LOG_INF("HLT", "Restored quotes from .bak after promote failure");
      }
    }
    return;
  }

  LOG_DBG("HLT", "Quote saved to %s", quotesPath.c_str());
}

// ── End Highlight / Quote selection mode ─────────────────────────────────────

void EpubReaderActivity::renderContents(const Page& page, const int orientedMarginTop, const int orientedMarginRight,
                                        const int orientedMarginBottom, const int orientedMarginLeft,
                                        const StatusBarLayout& statusBarLayout) {
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.setTextRenderStyle(SETTINGS.textRenderMode);

  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  // Vertically center text on full pages to distribute leftover space evenly
  // between top and bottom, so the visual gap matches the configured margins.
  // Only text-only full pages qualify — partial (last) pages stay top-aligned.
  int contentY = orientedMarginTop;
  if (page.isTextOnly()) {
    const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
    if (lineHeight > 0) {
      const int usedHeight = page.getUsedHeight(lineHeight);
      if (usedHeight > 0 && usedHeight < viewportHeight) {
        const int slack = viewportHeight - usedHeight;
        // Only center if slack is less than one line — that means
        // the page is full (just has rounding leftover).
        if (slack < lineHeight) {
          contentY += slack / 2;
        }
      }
    }
  }

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
  if (highlights_.state() != HighlightState::NONE) {
    renderHighlights(page, SETTINGS.getReaderFontId(), orientedMarginLeft, contentY);
    // Draw dashed border around text area to indicate highlight mode.
    constexpr int frameOffset = 6;     // padding from text area to the frame
    constexpr int frameThickness = 5;  // thicker frame for visibility
    const int bx = orientedMarginLeft - frameOffset;
    const int by = contentY - frameOffset;
    const int bw = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight + 2 * frameOffset;
    const int bh = viewportHeight + 2 * frameOffset;
    drawDashedRect(renderer, bx, by, bw, bh, frameThickness);
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
  ReaderStatusBar::renderStatusBar(renderer, statusBarLayout, orientedMarginRight, orientedMarginBottom,
                                   orientedMarginLeft, SETTINGS.debugBorders);
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
